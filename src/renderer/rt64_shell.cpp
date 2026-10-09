#include "jfg/runtime/snapshot_copy.hpp"
#include "jfg/renderer/rt64_shell.hpp"
#include "jfg/renderer/vi_presentation.hpp"
#include "jfg/renderer/widescreen.hpp"

#include "rt64_f3ddkr.hpp"

#include "hle/rt64_application.h"
#include "hle/rt64_vi.h"
#include "rhi/rt64_render_hooks.h"

#include "xxHash/xxhash.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace jfg {
namespace {

constexpr std::size_t kRspMemoryBytes = 4096U;
constexpr std::size_t kMaximumUcodeBytes = 4096U;
constexpr std::size_t kMaximumUcodeDataBytes = 2048U;

void ignore_rt64_interrupts() {}

[[nodiscard]] bool address_range_valid(
    const std::uint32_t address,
    const std::size_t length) noexcept {
    const std::size_t offset = address & 0x00FF'FFFFU;
    return offset <= kRt64RequiredRdramBytes &&
        length <= kRt64RequiredRdramBytes - offset;
}

[[nodiscard]] Rt64ShellError setup_error(
    const RT64::Application::SetupResult result) noexcept {
    switch (result) {
    case RT64::Application::SetupResult::Success:
        return Rt64ShellError::none;
    case RT64::Application::SetupResult::DynamicLibrariesNotFound:
        return Rt64ShellError::setup_dynamic_libraries_missing;
    case RT64::Application::SetupResult::InvalidGraphicsAPI:
        return Rt64ShellError::setup_invalid_graphics_api;
    case RT64::Application::SetupResult::GraphicsAPINotFound:
        return Rt64ShellError::setup_graphics_api_not_found;
    case RT64::Application::SetupResult::GraphicsDeviceNotFound:
        return Rt64ShellError::setup_graphics_device_not_found;
    }
    return Rt64ShellError::renderer_exception;
}

}  // namespace

struct CaptureContext;
std::atomic<CaptureContext*> active_capture = nullptr;

struct CaptureContext {
    RT64::Application* application = nullptr;
    RenderDevice* device = nullptr;
    RT64::RenderHookInit* previous_init = nullptr;
    RT64::RenderHookDraw* previous_draw = nullptr;
    RT64::RenderHookDeinit* previous_deinit = nullptr;
    std::unique_ptr<RenderBuffer> readback;
    // Written by the game thread, read by RT64's present thread.
    std::atomic<float> horizontal_extent = 1.0F;
    std::vector<std::byte> pixels;
    std::size_t width = 0U;
    std::size_t height = 0U;
    std::size_t row_pitch = 0U;
    bool armed = false;
    bool scheduled = false;
    bool failed = false;
    std::mutex mutex;

    [[nodiscard]] bool activate() noexcept;
    void deactivate() noexcept;
    void arm() noexcept;
    void draw(RenderCommandList* list, RenderFramebuffer* framebuffer) noexcept;
    [[nodiscard]] bool finish() noexcept;
};

void capture_init(RenderInterface* render_interface, RenderDevice* device) {
    CaptureContext* context = active_capture.load();
    if (context == nullptr) {
        return;
    }
    if (context->previous_init != nullptr) {
        context->previous_init(render_interface, device);
    }
    const std::scoped_lock lock(context->mutex);
    context->device = device;
}

void capture_draw(RenderCommandList* list, RenderFramebuffer* framebuffer) {
    CaptureContext* context = active_capture.load();
    if (context == nullptr) {
        return;
    }
    if (context->previous_draw != nullptr) {
        context->previous_draw(list, framebuffer);
    }
    context->draw(list, framebuffer);
}

void capture_deinit() {
    CaptureContext* context = active_capture.load();
    if (context == nullptr) {
        return;
    }
    if (context->previous_deinit != nullptr) {
        context->previous_deinit();
    }
    const std::scoped_lock lock(context->mutex);
    context->device = nullptr;
}

void clear_vi_guard_texel(
    RT64::Application* application,
    RenderCommandList* list,
    const float horizontal_extent) noexcept {
    if (application == nullptr || application->swapChain == nullptr ||
        list == nullptr) {
        return;
    }

    const RT64::VI vi = application->core.decodeVI();
    const hlslpp::uint2 framebuffer_size = vi.fbSize();
    const std::uint32_t window_width = application->swapChain->getWidth();
    const std::uint32_t window_height = application->swapChain->getHeight();
    if (!vi.visible() || framebuffer_size.x == 0U ||
        framebuffer_size.y == 0U || window_width == 0U ||
        window_height == 0U) {
        return;
    }

    // RT64 deliberately allocates filtering guard texels beyond the N64's
    // useful framebuffer extent. JFG leaves the lower/right guard texel
    // undefined, so linear VI sampling otherwise repeats it as a colored
    // edge. Clip exactly one source texel after VI scaling without touching
    // guest RDRAM or the rendered image interior.
    const auto display = jfg::vi_presentation_size(vi.hRegion.word, vi.vRegion.word);
    if (!display.valid()) return;
    // RGO widescreen presents the scanout this much wider than its own shape.
    const double extent = (std::isfinite(horizontal_extent) &&
        horizontal_extent > 1.0F) ? horizontal_extent : 1.0;
    const double display_width = display.width * extent;
    const double scale = (std::min)(
        static_cast<double>(window_width) / display_width,
        static_cast<double>(window_height) / display.height);
    if (!std::isfinite(scale) || scale <= 0.0) return;
    const double rendered_width = display_width * scale;
    const double rendered_height = display.height * scale;
    const std::int32_t left = static_cast<std::int32_t>(std::lround(
        (static_cast<double>(window_width) - rendered_width) * 0.5));
    const std::int32_t top = static_cast<std::int32_t>(std::lround(
        (static_cast<double>(window_height) - rendered_height) * 0.5));
    const std::int32_t right = static_cast<std::int32_t>(std::lround(left + rendered_width));
    const std::int32_t bottom = static_cast<std::int32_t>(std::lround(top + rendered_height));
    const std::int32_t guard_x = static_cast<std::int32_t>(std::ceil(
        rendered_width / (static_cast<double>(framebuffer_size.x) * extent)));
    const std::int32_t guard_y = static_cast<std::int32_t>(std::ceil(rendered_height / framebuffer_size.y));
    const std::int32_t guard = (std::max)(guard_x, guard_y);
    if (right <= left || bottom <= top || guard <= 0) {
        return;
    }

    const std::array<RenderRect, 2U> guard_rectangles{
        RenderRect((std::max)(left, right - guard_x), top, right, bottom),
        RenderRect(left, (std::max)(top, bottom - guard_y), right, bottom),
    };
    list->clearColor(
        0U,
        RenderColor(),
        guard_rectangles.data(),
        static_cast<std::uint32_t>(guard_rectangles.size()));
}

bool CaptureContext::activate() noexcept {
    CaptureContext* expected = nullptr;
    if (!active_capture.compare_exchange_strong(expected, this)) {
        return false;
    }
    previous_init = RT64::GetRenderHookInit();
    previous_draw = RT64::GetRenderHookDraw();
    previous_deinit = RT64::GetRenderHookDeinit();
    RT64::SetRenderHooks(&capture_init, &capture_draw, &capture_deinit);
    return true;
}

void CaptureContext::deactivate() noexcept {
    CaptureContext* expected = this;
    if (active_capture.compare_exchange_strong(expected, nullptr)) {
        RT64::SetRenderHooks(previous_init, previous_draw, previous_deinit);
    }
}

void CaptureContext::arm() noexcept {
    const std::scoped_lock lock(mutex);
    pixels.clear();
    armed = true;
    scheduled = false;
    failed = false;
}

void CaptureContext::draw(
    RenderCommandList* list,
    RenderFramebuffer* framebuffer) noexcept {
    const std::scoped_lock lock(mutex);
    clear_vi_guard_texel(application, list, horizontal_extent.load());
    if (!armed || scheduled || failed || application == nullptr ||
        application->swapChain == nullptr ||
        application->presentQueue == nullptr || device == nullptr ||
        list == nullptr || framebuffer == nullptr) {
        return;
    }
    try {
        std::size_t texture_index =
            application->presentQueue->swapChainFramebuffers.size();
        for (std::size_t index = 0U;
             index < application->presentQueue->swapChainFramebuffers.size();
             ++index) {
            if (application->presentQueue->swapChainFramebuffers[index].get() ==
                framebuffer) {
                texture_index = index;
                break;
            }
        }
        if (texture_index >= application->swapChain->getTextureCount()) {
            failed = true;
            return;
        }
        RenderTexture* texture = application->swapChain->getTexture(
            static_cast<std::uint32_t>(texture_index));
        width = application->swapChain->getWidth();
        height = application->swapChain->getHeight();
        if (texture == nullptr || width == 0U || height == 0U) {
            failed = true;
            return;
        }
        row_pitch = ((width * 4U + 255U) / 256U) * 256U;
        const std::size_t byte_count = row_pitch * height;
        readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(
            byte_count));
        if (readback == nullptr) {
            failed = true;
            return;
        }
        const std::array<RenderBufferBarrier, 1U> buffer_barriers{
            RenderBufferBarrier(readback.get(), RenderBufferAccess::WRITE),
        };
        list->barriers(RenderBarrierStage::COPY,
            buffer_barriers.data(),
            static_cast<std::uint32_t>(buffer_barriers.size()));
        list->barriers(RenderBarrierStage::COPY,
            RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(
                readback.get(),
                RenderFormat::B8G8R8A8_UNORM,
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                1U,
                static_cast<std::uint32_t>(row_pitch / 4U)),
            RenderTextureCopyLocation::Subresource(texture));
        list->barriers(RenderBarrierStage::ALL,
            RenderTextureBarrier(texture, RenderTextureLayout::COLOR_WRITE));
        scheduled = true;
    }
    catch (...) {
        failed = true;
    }
}

bool CaptureContext::finish() noexcept {
    const std::scoped_lock lock(mutex);
    armed = false;
    if (!scheduled || failed || readback == nullptr || width == 0U ||
        height == 0U || row_pitch > SIZE_MAX / height) {
        return false;
    }
    try {
        const std::size_t byte_count = row_pitch * height;
        const RenderRange range(0U, byte_count);
        const auto* mapped = static_cast<const std::byte*>(
            readback->map(0U, &range));
        if (mapped == nullptr) {
            return false;
        }
        try {
            pixels.assign(mapped, mapped + byte_count);
        }
        catch (...) {
            readback->unmap();
            throw;
        }
        readback->unmap();
        return true;
    }
    catch (...) {
        pixels.clear();
        return false;
    }
}

struct Rt64Shell::Impl {
    std::array<std::uint8_t, kRt64RequiredHeaderBytes> header{};
    std::vector<std::uint8_t> rdram =
        std::vector<std::uint8_t>(kRt64RequiredRdramBytes);
    std::vector<std::uint8_t> simulation_rdram =
        std::vector<std::uint8_t>(kRt64RequiredRdramBytes);
    std::vector<std::uint8_t> task_rdram_copy =
        std::vector<std::uint8_t>(kRt64RequiredRdramBytes);
    std::span<const std::uint8_t> task_rdram{};
    std::array<std::uint8_t, kRspMemoryBytes> dmem{};
    std::array<std::uint8_t, kRspMemoryBytes> imem{};
    std::uint32_t mi_interrupt = 0U;
    std::array<std::uint32_t, 8U> dpc{};
    std::unique_ptr<RT64::Application> application;
    std::unique_ptr<Rt64F3ddkr> f3ddkr;
    std::unique_ptr<CaptureContext> capture;
    Rt64FramebufferInfo last_framebuffer;
    bool developer_mode = false;
    bool initialized = false;
    bool setup_started = false;
    bool cpu_writeback = false;
    bool cpu_mask_writeback = false;
    bool writeback_pending = false;
    std::vector<Rt64RdramRange> writeback_ranges;
    std::uint64_t last_rdram_check_microseconds = 0U;
    bool widescreen = false;
    float horizontal_extent = 1.0F;

    // Choose RT64's aspect target for the current window and VI mode.
    void update_widescreen() {
        RT64::Application& app = *application;
        const RT64::VI vi = app.core.decodeVI();
        const hlslpp::uint2 framebuffer_size = vi.fbSize();
        const auto display =
            vi_presentation_size(vi.hRegion.word, vi.vRegion.word);
        std::uint32_t window_width = 0U;
        std::uint32_t window_height = 0U;
        {
            auto& shared = *app.sharedQueueResources;
            const std::scoped_lock lock(shared.configurationMutex);
            window_width = shared.swapChainWidth;
            window_height = shared.swapChainHeight;
        }
        double target = 0.0;
        float extent = 1.0F;
        if (widescreen && display.valid()) {
            target = widescreen_aspect_target(window_width, window_height,
                framebuffer_size.x, framebuffer_size.y,
                display.width, display.height);
            extent = widescreen_horizontal_extent(
                target, framebuffer_size.x, framebuffer_size.y);
        }
        RT64::UserConfiguration& config = app.userConfig;
        const bool expand = extent > 1.0F;
        const auto mode = expand ? RT64::UserConfiguration::AspectRatio::Manual
                                 : RT64::UserConfiguration::AspectRatio::Original;
        if (config.aspectRatio != mode ||
            (expand && std::abs(config.aspectTarget - target) > 1e-4)) {
            config.aspectRatio = mode;
            if (expand)
                config.aspectTarget = target;
            app.updateUserConfig(false);
        }
        horizontal_extent = extent;
        f3ddkr->set_horizontal_extent(extent);
        capture->horizontal_extent.store(extent);
    }

    ~Impl() {
        f3ddkr.reset();
        if (setup_started && application != nullptr) {
            application->end();
        }
        if (capture != nullptr) {
            capture->deactivate();
        }
    }
};

Rt64ShellError validate_rt64_shell_configuration(
    const Rt64ShellConfiguration& configuration) noexcept {
    if (configuration.native_window == nullptr) {
        return Rt64ShellError::invalid_window;
    }
    if (configuration.rom_header.size() < kRt64RequiredHeaderBytes ||
        configuration.rdram.size() != kRt64RequiredRdramBytes ||
        configuration.vi == nullptr) {
        return Rt64ShellError::invalid_memory;
    }
    return Rt64ShellError::none;
}

Rt64ShellError copy_rt64_rdram_snapshot(
    const std::span<const std::byte> source,
    const std::span<std::byte> destination,
    const Rt64MemoryLayout layout) noexcept {
    if (source.size() != kRt64RequiredRdramBytes ||
        destination.size() != kRt64RequiredRdramBytes) {
        return Rt64ShellError::invalid_memory;
    }
    if (layout == Rt64MemoryLayout::host_word_swapped) {
        copy_changed_snapshot_bytes(source, destination);
        return Rt64ShellError::none;
    }
    for (std::size_t index = 0U; index < source.size(); index += 4U) {
        destination[index] = source[index + 3U];
        destination[index + 1U] = source[index + 2U];
        destination[index + 2U] = source[index + 1U];
        destination[index + 3U] = source[index];
    }
    return Rt64ShellError::none;
}

Rt64ShellError merge_rt64_rdram_snapshot(
    const std::span<const std::byte> source,
    const std::span<std::byte> destination,
    const std::span<std::byte> previous_source,
    const Rt64MemoryLayout layout) noexcept {
    if (source.size() != kRt64RequiredRdramBytes ||
        destination.size() != kRt64RequiredRdramBytes ||
        previous_source.size() != kRt64RequiredRdramBytes) {
        return Rt64ShellError::invalid_memory;
    }
    if (layout == Rt64MemoryLayout::host_word_swapped) {
        constexpr std::size_t kComparisonBlockBytes = 4096U;
        for (std::size_t base = 0U; base < source.size();
             base += kComparisonBlockBytes) {
            if (std::memcmp(source.data() + base,
                            previous_source.data() + base,
                            kComparisonBlockBytes) == 0) {
                continue;
            }
#if defined(_M_X64) || defined(__SSE2__)
            // Equal CPU bytes preserve private GPU results. Select each byte
            // independently, including mixed CPU/GPU ownership in one vector.
            for (std::size_t index = base;
                 index < base + kComparisonBlockBytes; index += 16U) {
                const auto incoming = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(source.data() + index));
                const auto previous = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(previous_source.data() + index));
                const auto current = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(destination.data() + index));
                const auto unchanged = _mm_cmpeq_epi8(incoming, previous);
                const auto merged = _mm_or_si128(
                    _mm_and_si128(unchanged, current),
                    _mm_andnot_si128(unchanged, incoming));
                _mm_storeu_si128(
                    reinterpret_cast<__m128i*>(destination.data() + index), merged);
                _mm_storeu_si128(
                    reinterpret_cast<__m128i*>(previous_source.data() + index), incoming);
            }
#else
            for (std::size_t index = base;
                 index < base + kComparisonBlockBytes; ++index) {
                const std::byte incoming = source[index];
                if (incoming != previous_source[index]) {
                    destination[index] = incoming;
                    previous_source[index] = incoming;
                }
            }
#endif
        }
        return Rt64ShellError::none;
    }
    for (std::size_t index = 0U; index < source.size(); ++index) {
        const std::byte incoming = source[index ^ 3U];
        if (incoming != previous_source[index]) {
            destination[index] = incoming;
            previous_source[index] = incoming;
        }
    }
    return Rt64ShellError::none;
}

Rt64ShellError refresh_rt64_cpu_memory(
    const std::span<const std::byte> source,
    const std::span<std::byte> destination,
    const std::span<const Rt64RdramRange> gpu_ranges,
    const Rt64MemoryLayout layout) noexcept {
    if (source.size() != kRt64RequiredRdramBytes || destination.size() != source.size())
        return Rt64ShellError::invalid_memory;
    std::size_t previous_begin = 0U;
    for (const auto range : gpu_ranges) {
        if (range.begin >= range.end || range.end > source.size() || range.begin < previous_begin)
            return Rt64ShellError::invalid_memory;
        previous_begin = range.begin;
    }
    const auto copy_gap = [&](std::size_t begin, const std::size_t end) {
        if (layout == Rt64MemoryLayout::big_endian) {
            for (; begin < end; ++begin)
                destination[begin ^ 3U] = source[begin];
            return;
        }
        // Physical ranges use N64 byte addresses, including unaligned edges.
        for (; begin < end && (begin & 3U) != 0U; ++begin)
            destination[begin ^ 3U] = source[begin ^ 3U];
        const std::size_t aligned_end = end & ~std::size_t{3U};
        if (aligned_end > begin) {
            // Compare against actual renderer RAM, never a previous CPU-only
            // snapshot: framebuffer reuse can leave changed GPU bytes here.
            constexpr std::size_t block = 4096U;
            while (aligned_end - begin >= block) {
                if (std::memcmp(destination.data() + begin, source.data() + begin, block) != 0)
                    std::memcpy(destination.data() + begin, source.data() + begin, block);
                begin += block;
            }
            if (aligned_end > begin &&
                std::memcmp(destination.data() + begin, source.data() + begin,
                            aligned_end - begin) != 0)
                std::memcpy(destination.data() + begin, source.data() + begin, aligned_end - begin);
            begin = aligned_end;
        }
        for (; begin < end; ++begin)
            destination[begin ^ 3U] = source[begin ^ 3U];
    };
    std::size_t cursor = 0U;
    for (const auto range : gpu_ranges) {
        if (range.begin > cursor) copy_gap(cursor, range.begin);
        cursor = (std::max)(cursor, range.end);
    }
    if (cursor < source.size()) copy_gap(cursor, source.size());
    return Rt64ShellError::none;
}

Rt64ShellError commit_rt64_rdram_ranges(
    const std::span<const std::byte> submitted,
    const std::span<const std::byte> rendered,
    const std::span<std::byte> live,
    const std::span<const Rt64RdramRange> ranges,
    const std::span<std::byte> previous_source) noexcept {
    if (submitted.size() != kRt64RequiredRdramBytes ||
        rendered.size() != submitted.size() || live.empty() ||
        live.size() > submitted.size() || live.size() % 4U != 0U ||
        (!previous_source.empty() && previous_source.size() != submitted.size()))
        return Rt64ShellError::invalid_memory;
    const auto equal_range = [&](const Rt64RdramRange range) {
        auto address=range.begin;
        for (; address<range.end && (address&3U); ++address)
            if(live[address^3U]!=submitted[address^3U]) return false;
        const auto aligned_end=range.end & ~std::size_t{3U};
        if(address<aligned_end) {
            if(std::memcmp(live.data()+address,submitted.data()+address,aligned_end-address)!=0) return false;
            address=aligned_end;
        }
        for(;address<range.end;++address)
            if(live[address^3U]!=submitted[address^3U]) return false;
        return true;
    };
    for (const auto range : ranges) {
        if (range.begin >= range.end || range.end > live.size())
            return Rt64ShellError::invalid_memory;
        if(!equal_range(range)) return Rt64ShellError::conflicting_cpu_write;
    }
    for (const auto range : ranges) {
        auto address=range.begin;
        const auto copy_byte=[&](const std::size_t i) {
            live[i^3U]=rendered[i^3U];
            if(!previous_source.empty())previous_source[i^3U]=rendered[i^3U];
        };
        for(;address<range.end && (address&3U);++address)copy_byte(address);
        const auto aligned_end=range.end & ~std::size_t{3U};
        if(address<aligned_end) {
            if (live.data() != rendered.data())
                std::memcpy(live.data()+address,rendered.data()+address,aligned_end-address);
            if (!previous_source.empty() && previous_source.data() != rendered.data())
                std::memcpy(previous_source.data()+address,rendered.data()+address,aligned_end-address);
            address=aligned_end;
        }
        for(;address<range.end;++address)copy_byte(address);
    }
    return Rt64ShellError::none;
}

Rt64ShellError inspect_rt64_framebuffer(
    const Rt64ViRegisters& vi,
    const std::size_t rdram_size,
    Rt64FramebufferInfo& framebuffer) noexcept {
    framebuffer = {};
    const std::uint32_t pixel_type = vi.status & 0x3U;
    const std::size_t bytes_per_pixel = pixel_type == 2U
        ? 2U
        : pixel_type == 3U ? 4U : 0U;
    const std::size_t vi_width = vi.width & 0xFFFU;
    const std::uint32_t horizontal_start =
        (vi.horizontal_start >> 16U) & 0x3FFU;
    const std::uint32_t horizontal_end = vi.horizontal_start & 0x3FFU;
    const std::uint32_t vertical_start =
        (vi.vertical_start >> 16U) & 0x3FFU;
    const std::uint32_t vertical_end = vi.vertical_start & 0x3FFU;
    const std::uint32_t x_scale = vi.x_scale & 0xFFFU;
    const std::uint32_t y_scale = vi.y_scale & 0xFFFU;
    if (bytes_per_pixel == 0U || vi_width == 0U ||
        horizontal_start == 0U || horizontal_end <= horizontal_start ||
        vertical_end <= vertical_start || x_scale == 0U || y_scale == 0U) {
        return Rt64ShellError::invalid_framebuffer;
    }

    std::size_t framebuffer_width = vi_width;
    const bool serrate = (vi.status & (1U << 6U)) != 0U;
    const double estimated_width =
        static_cast<double>(horizontal_end - horizontal_start) *
        static_cast<double>(x_scale) / 1024.0;
    if (serrate && estimated_width <
            static_cast<double>(vi_width) / 1.875) {
        framebuffer_width = vi_width / 2U;
    }
    if (framebuffer_width == 0U) {
        return Rt64ShellError::invalid_framebuffer;
    }

    const double estimated_height =
        static_cast<double>(vertical_end - vertical_start) *
        static_cast<double>(y_scale) * static_cast<double>(vi_width) /
        (2048.0 * static_cast<double>(framebuffer_width));
    if (!std::isfinite(estimated_height) || estimated_height <= 0.0 ||
        estimated_height > static_cast<double>(SIZE_MAX - 2U)) {
        return Rt64ShellError::invalid_framebuffer;
    }
    const double expanded_height = std::round(estimated_height) + 2.0;
    const double aligned_height = std::round(expanded_height / 4.0) * 4.0;
    if (!std::isfinite(aligned_height) || aligned_height <= 0.0 ||
        aligned_height > static_cast<double>(SIZE_MAX)) {
        return Rt64ShellError::invalid_framebuffer;
    }
    const std::size_t framebuffer_height =
        static_cast<std::size_t>(aligned_height);

    if (vi_width > SIZE_MAX / bytes_per_pixel) {
        return Rt64ShellError::invalid_framebuffer;
    }
    const std::size_t row_bytes = vi_width * bytes_per_pixel;
    const std::size_t row_count = serrate && (vi.current_line & 1U) != 0U
        ? 2U
        : 1U;
    if (row_bytes > SIZE_MAX / row_count) {
        return Rt64ShellError::invalid_framebuffer;
    }
    const std::size_t row_offset = row_bytes * row_count;
    std::size_t address = vi.origin & 0x00FF'FFFFU;
    if (address >= row_offset) {
        address -= row_offset;
    }

    framebuffer = {
        static_cast<std::uint32_t>(address),
        framebuffer_width,
        framebuffer_height,
        bytes_per_pixel,
    };
    const std::size_t byte_count = framebuffer.byte_count();
    if (byte_count == 0U || address > rdram_size ||
        byte_count > rdram_size - address) {
        framebuffer = {};
        return Rt64ShellError::invalid_framebuffer;
    }
    return Rt64ShellError::none;
}

Rt64Shell::Rt64Shell(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Rt64Shell::~Rt64Shell() = default;

Rt64ShellError Rt64Shell::replace_rdram_snapshot(
    const std::span<const std::byte> rdram,
    const Rt64MemoryLayout layout) noexcept {
    if (impl_ == nullptr || !impl_->initialized ||
        impl_->application == nullptr) {
        return Rt64ShellError::not_initialized;
    }
    if (impl_->writeback_pending)
        return Rt64ShellError::conflicting_cpu_write;
    // Full CPU writeback completes before another task is admitted. At this
    // boundary guest RAM contains both CPU writes and committed GPU results.
    // Import it directly; byte-delta merging and GPU-range ownership scans
    // are needed only when private GPU results have not been written back.
    if (impl_->cpu_writeback) {
        const auto copied = copy_rt64_rdram_snapshot(rdram,
            std::as_writable_bytes(std::span(impl_->rdram)), layout);
        if (copied != Rt64ShellError::none) return copied;
        // Full writeback imports every source byte at the next task boundary.
        // The delta-import history is used only when full writeback is off.
    } else {
    const Rt64ShellError merge_error = merge_rt64_rdram_snapshot(
        rdram,
        std::as_writable_bytes(std::span(impl_->rdram)),
        std::as_writable_bytes(std::span(impl_->simulation_rdram)),
        layout);
    if (merge_error != Rt64ShellError::none) {
        return merge_error;
    }
    // Framebuffer storage can be recycled for CPU textures. A byte-only
    // delta import misses same-value CPU writes after RT64 changed that RAM.
    // Preserve private GPU results only while a live framebuffer owns them.
    std::vector<Rt64RdramRange> gpu_ranges;
    try {
        for (const auto& [address, framebuffer] : impl_->application->state->framebufferManager.framebuffers) {
            (void)address;
            if (framebuffer.lastWriteType != RT64::Framebuffer::Type::None &&
                framebuffer.addressStart < framebuffer.addressEnd)
                gpu_ranges.push_back({framebuffer.addressStart, framebuffer.addressEnd});
        }
        std::sort(gpu_ranges.begin(), gpu_ranges.end(), [](const auto left, const auto right) {
            return left.begin < right.begin;
        });
    }
    catch (...) { return Rt64ShellError::renderer_exception; }
    const auto refresh_error = refresh_rt64_cpu_memory(rdram,
        std::as_writable_bytes(std::span(impl_->rdram)), gpu_ranges, layout);
    if (refresh_error != Rt64ShellError::none) return refresh_error;
    }
    if (layout == Rt64MemoryLayout::host_word_swapped) {
        impl_->task_rdram = {
            reinterpret_cast<const std::uint8_t*>(rdram.data()),
            rdram.size(),
        };
        return Rt64ShellError::none;
    }
    const Rt64ShellError task_copy_error = copy_rt64_rdram_snapshot(
        rdram,
        std::as_writable_bytes(std::span(impl_->task_rdram_copy)),
        layout);
    if (task_copy_error == Rt64ShellError::none) {
        impl_->task_rdram = impl_->task_rdram_copy;
    }
    return task_copy_error;
}

Rt64ShellError Rt64Shell::copy_private_rdram_snapshot(
    const std::span<std::byte> rdram,
    const Rt64MemoryLayout layout) const noexcept {
    if (impl_ == nullptr || !impl_->initialized ||
        impl_->application == nullptr) {
        return Rt64ShellError::not_initialized;
    }
    return copy_rt64_rdram_snapshot(
        std::as_bytes(std::span(impl_->rdram)), rdram, layout);
}

std::unique_ptr<Rt64Shell> Rt64Shell::create(
    const Rt64ShellConfiguration& configuration,
    Rt64ShellError& error) noexcept {
    error = validate_rt64_shell_configuration(configuration);
    if (error != Rt64ShellError::none) {
        return nullptr;
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->developer_mode = configuration.developer_mode;
        impl->cpu_writeback = configuration.cpu_writeback;
        impl->cpu_mask_writeback = configuration.cpu_mask_writeback;
        for (std::size_t index = 0U; index < impl->header.size(); ++index) {
            impl->header[index] = std::to_integer<std::uint8_t>(
                configuration.rom_header[index]);
        }
        const Rt64ShellError snapshot_error = copy_rt64_rdram_snapshot(
            configuration.rdram,
            std::as_writable_bytes(std::span(impl->rdram)),
            configuration.memory_layout);
        if (snapshot_error != Rt64ShellError::none) {
            error = snapshot_error;
            return nullptr;
        }
        impl->simulation_rdram = impl->rdram;
        impl->task_rdram_copy = impl->rdram;
        impl->task_rdram = impl->task_rdram_copy;

        RT64::Application::Core core{};
        core.window = static_cast<RenderWindow>(configuration.native_window);
        core.HEADER = impl->header.data();
        core.RDRAM = impl->rdram.data();
        core.DMEM = impl->dmem.data();
        core.IMEM = impl->imem.data();
        core.MI_INTR_REG = &impl->mi_interrupt;
        core.DPC_START_REG = &impl->dpc[0U];
        core.DPC_END_REG = &impl->dpc[1U];
        core.DPC_CURRENT_REG = &impl->dpc[2U];
        core.DPC_STATUS_REG = &impl->dpc[3U];
        core.DPC_CLOCK_REG = &impl->dpc[4U];
        core.DPC_BUFBUSY_REG = &impl->dpc[5U];
        core.DPC_PIPEBUSY_REG = &impl->dpc[6U];
        core.DPC_TMEM_REG = &impl->dpc[7U];
        core.VI_STATUS_REG = &configuration.vi->status;
        core.VI_ORIGIN_REG = &configuration.vi->origin;
        core.VI_WIDTH_REG = &configuration.vi->width;
        core.VI_INTR_REG = &configuration.vi->intr;
        core.VI_V_CURRENT_LINE_REG = &configuration.vi->current_line;
        core.VI_TIMING_REG = &configuration.vi->timing;
        core.VI_V_SYNC_REG = &configuration.vi->vertical_sync;
        core.VI_H_SYNC_REG = &configuration.vi->horizontal_sync;
        core.VI_LEAP_REG = &configuration.vi->leap;
        core.VI_H_START_REG = &configuration.vi->horizontal_start;
        core.VI_V_START_REG = &configuration.vi->vertical_start;
        core.VI_V_BURST_REG = &configuration.vi->vertical_burst;
        core.VI_X_SCALE_REG = &configuration.vi->x_scale;
        core.VI_Y_SCALE_REG = &configuration.vi->y_scale;
        // RT64 raises SP internally at the end of processDisplayLists. The
        // project bridge owns the externally visible completion commit, after
        // parsing and oracle validation, so this callback must remain isolated.
        core.checkInterrupts = &ignore_rt64_interrupts;

        RT64::ApplicationConfiguration app_configuration;
        app_configuration.appId = "jfg-recomp";
        app_configuration.dataPath = std::filesystem::path{};
        app_configuration.detectDataPath = false;
        app_configuration.useConfigurationFile = false;
        impl->application =
            std::make_unique<RT64::Application>(core, app_configuration);
        if (configuration.cpu_mask_writeback) {
            // JFG softens rendered shadow masks on the CPU before sampling them.
            // A cached GPU framebuffer copy bypasses those edits. Load textures
            // from the updated RAM when the game owns mask postprocessing.
            impl->application->emulatorConfig.framebuffer.copyWithGPU = false;
        }
        impl->capture = std::make_unique<CaptureContext>();
        impl->capture->application = impl->application.get();
        if (!impl->capture->activate()) {
            error = Rt64ShellError::render_hook_unavailable;
            return nullptr;
        }
        impl->application->userConfig.developerMode =
            configuration.developer_mode;
        // JFG draws CPU overlays into the buffer selected by the N64 VI.
        // Skipping to a newer rendered buffer can display it before those edits.
        impl->application->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::Console;
        impl->setup_started = true;
        const Rt64ShellError result = setup_error(
            impl->application->setup(configuration.window_thread_id));
        if (result != Rt64ShellError::none) {
            error = result;
            return nullptr;
        }
        if (impl->application->state == nullptr ||
            impl->application->interpreter == nullptr ||
            impl->application->workloadQueue == nullptr ||
            impl->application->presentQueue == nullptr) {
            error = Rt64ShellError::setup_dynamic_libraries_missing;
            return nullptr;
        }
        impl->initialized = true;
        impl->f3ddkr = std::make_unique<Rt64F3ddkr>(*impl->application);
        error = Rt64ShellError::none;
        return std::unique_ptr<Rt64Shell>(
            new Rt64Shell(std::move(impl)));
    }
    catch (...) {
        error = Rt64ShellError::renderer_exception;
        return nullptr;
    }
}

Rt64ShellError Rt64Shell::submit(const Rt64GraphicsTask& task) noexcept {
    if (impl_ == nullptr || !impl_->initialized ||
        impl_->application == nullptr) {
        return Rt64ShellError::not_initialized;
    }
    if (!address_range_valid(task.ucode_address, kMaximumUcodeBytes) ||
        !address_range_valid(task.ucode_data_address, kMaximumUcodeDataBytes) ||
        !address_range_valid(task.command_address, sizeof(std::uint64_t))) {
        return Rt64ShellError::invalid_task_address;
    }

    try {
        RT64::Application& application = *impl_->application;
        if (impl_->writeback_pending)
            return Rt64ShellError::conflicting_cpu_write;
        impl_->update_widescreen();
        const auto previous_write = application.state->framebufferManager.writeTimestamp;
        impl_->writeback_ranges.clear();
        application.state->rsp->reset();
        if (impl_->task_rdram.size() != kRt64RequiredRdramBytes) {
            return Rt64ShellError::invalid_memory;
        }
        // CPU post-processing of offscreen results must be visible to texture
        // loads. GPU framebuffer reuse can otherwise bypass those RAM writes.
        if (impl_->cpu_writeback)
            impl_->application->emulatorConfig.framebuffer.copyWithGPU = false;
        impl_->f3ddkr->begin(impl_->task_rdram);
        impl_->f3ddkr->install();
        const auto rdram_check_start = std::chrono::steady_clock::now();
        // Refresh CPU-owned intensity buffers when they are next drawn. Texture
        // loads already use RAM; invalidate render contents for the next draw
        // without allocating a GPU pixel-difference map for an unused target.
        if (impl_->cpu_writeback && application.state->rdramCheckPending &&
            !application.emulatorConfig.framebuffer.copyWithGPU) {
            auto &manager = application.state->framebufferManager;
            std::vector<RT64::Framebuffer *> changed;
            manager.checkRAM(application.state->RDRAM, changed, false);
            for (auto *fb : changed) {
                if (fb->lastWriteType == RT64::Framebuffer::Type::Color &&
                    fb->lastWriteFmt == G_IM_FMT_I && fb->siz == G_IM_SIZ_8b) {
                    fb->RAMHash = XXH3_64bits(application.state->RDRAM + fb->addressStart, fb->RAMBytes);
                    fb->rdramChanged = true;
                    fb->readHeight = 0U;
                }
            }
        }
        // Full-clear elision: only this exact self-contained
        // prefix establishes fill mode, I8 target and complete 64x64 clear.
        // No prior target pixels can survive or be sampled before the clear.
        const auto list = task.command_address & 0x00FFFFFFU;
        if (impl_->cpu_writeback && !application.emulatorConfig.framebuffer.copyWithGPU &&
            list <= impl_->task_rdram.size() &&
            56U <= impl_->task_rdram.size() - list) {
            std::uint32_t w[14]{};
            std::memcpy(w, impl_->task_rdram.data() + list, sizeof(w));
            if (w[0] == 0xBC000006U && w[1] == 0x80000000U &&
                w[2] == 0xBC000406U && w[3] == w[7] &&
                w[4] == 0xBA001402U && w[5] == 0x00300000U &&
                w[6] == 0xFF88003FU && w[8] == 0xF7000000U &&
                w[9] == 0U && w[10] == 0xED000000U &&
                w[11] == 0x00100100U && w[12] == 0xF60FC0FCU &&
                w[13] == 0U && w[7] < kRt64RequiredRdramBytes) {
                auto &manager = application.state->framebufferManager;
                const auto found = manager.framebuffers.find(w[7]);
                if (found != manager.framebuffers.end()) {
                    auto &fb = found->second;
                    if (fb.lastWriteType == RT64::Framebuffer::Type::Color &&
                        fb.lastWriteFmt == G_IM_FMT_I && fb.siz == G_IM_SIZ_8b &&
                        fb.width == 64U && fb.height == 64U && fb.RAMBytes == 4096U &&
                        fb.addressEnd == w[7] + 4096U && fb.addressEnd <= kRt64RequiredRdramBytes) {
                        fb.RAMHash = XXH3_64bits(application.state->RDRAM + w[7], fb.RAMBytes);
                        // This complete fill replaces every old target pixel.
                        fb.rdramChanged = false;
                        fb.readHeight = fb.height;
                    }
                }
            }
        }
        application.state->checkRDRAM();
        impl_->last_rdram_check_microseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - rdram_check_start)
                    .count());
        application.processDisplayLists(
            const_cast<std::uint8_t*>(impl_->task_rdram.data()),
            task.command_address & 0x00FF'FFFFU,
            0U,
            true);
        if (!impl_->f3ddkr->complete()) {
            return Rt64ShellError::unsupported_commands;
        }
        if (impl_->cpu_writeback || impl_->cpu_mask_writeback) {
            // RT64's render-to-RAM full sync has finished its readback here.
            // Own whole touched framebuffer extents, including unchanged
            // pixels, so same-value GPU writes cannot hide CPU conflicts.
            for (const auto& [address, framebuffer] : application.state->framebufferManager.framebuffers) {
                (void)address;
                const bool cpu_mask =
                    framebuffer.lastWriteType == RT64::Framebuffer::Type::Color &&
                    framebuffer.lastWriteFmt == G_IM_FMT_I &&
                    framebuffer.siz == G_IM_SIZ_8b;
                if (framebuffer.lastWriteTimestamp > previous_write &&
                    (impl_->cpu_writeback || cpu_mask)) {
                    impl_->writeback_ranges.push_back(
                        {framebuffer.addressStart, framebuffer.addressEnd});
                }
            }
            impl_->writeback_pending = true;
        }
        return Rt64ShellError::none;
    }
    catch (...) {
        return Rt64ShellError::renderer_exception;
    }
}

Rt64ShellError Rt64Shell::commit_cpu_writeback(
    const std::span<const std::byte> submitted,
    const std::span<std::byte> live) noexcept {
    if (impl_ == nullptr || !impl_->initialized ||
        !(impl_->cpu_writeback || impl_->cpu_mask_writeback) ||
        !impl_->writeback_pending)
        return Rt64ShellError::not_initialized;
    const auto result = commit_rt64_rdram_ranges(submitted,
        std::as_bytes(std::span(impl_->rdram)), live, impl_->writeback_ranges,
        impl_->cpu_writeback ? std::span<std::byte>{}
            : std::as_writable_bytes(std::span(impl_->simulation_rdram)));
    if (result == Rt64ShellError::none) {
        impl_->writeback_pending = false;
        impl_->writeback_ranges.clear();
    }
    return result;
}

bool Rt64Shell::has_color_framebuffer(const std::uint32_t address) const noexcept {
    if (impl_ == nullptr || !impl_->initialized || impl_->application == nullptr ||
        impl_->application->state == nullptr || address >= impl_->rdram.size()) {
        return false;
    }
    const auto* framebuffer = impl_->application->state->framebufferManager.find(address);
    return framebuffer != nullptr &&
        framebuffer->lastWriteType == RT64::Framebuffer::Type::Color;
}

Rt64ShellError Rt64Shell::present(const bool capture_frame) noexcept {
    if (impl_ == nullptr || !impl_->initialized ||
        impl_->application == nullptr) {
        return Rt64ShellError::not_initialized;
    }
    const Rt64ShellError framebuffer_error = inspect_rt64_framebuffer(
        {
            *impl_->application->core.VI_STATUS_REG,
            *impl_->application->core.VI_ORIGIN_REG,
            *impl_->application->core.VI_WIDTH_REG,
            *impl_->application->core.VI_INTR_REG,
            *impl_->application->core.VI_V_CURRENT_LINE_REG,
            *impl_->application->core.VI_TIMING_REG,
            *impl_->application->core.VI_V_SYNC_REG,
            *impl_->application->core.VI_H_SYNC_REG,
            *impl_->application->core.VI_LEAP_REG,
            *impl_->application->core.VI_H_START_REG,
            *impl_->application->core.VI_V_START_REG,
            *impl_->application->core.VI_V_BURST_REG,
            *impl_->application->core.VI_X_SCALE_REG,
            *impl_->application->core.VI_Y_SCALE_REG,
        },
        impl_->rdram.size(),
        impl_->last_framebuffer);
    if (framebuffer_error != Rt64ShellError::none) {
        return framebuffer_error;
    }
    try {
        if (capture_frame)
            impl_->capture->arm();
        impl_->application->updateScreen();
        if (capture_frame) {
            impl_->application->presentQueue->waitForPresentId(
                impl_->application->state->presentId);
            impl_->application->presentQueue->waitForIdle();
        }
        if (capture_frame && !impl_->capture->finish()) {
            return Rt64ShellError::frame_capture_failed;
        }
        return Rt64ShellError::none;
    }
    catch (...) {
        return Rt64ShellError::renderer_exception;
    }
}

void Rt64Shell::set_widescreen(const bool enabled) noexcept {
    if (impl_ != nullptr)
        impl_->widescreen = enabled;
}

float Rt64Shell::horizontal_extent() const noexcept {
    return impl_ != nullptr ? impl_->horizontal_extent : 1.0F;
}

bool Rt64Shell::developer_debugger_available() const noexcept {
    return impl_ != nullptr && impl_->initialized && impl_->developer_mode;
}

std::size_t Rt64Shell::last_command_count() const noexcept {
    return impl_ != nullptr && impl_->f3ddkr != nullptr
        ? impl_->f3ddkr->command_count()
        : 0U;
}

Rt64GraphicsDiagnostics Rt64Shell::last_graphics_diagnostics()
    const noexcept {
    if (impl_ == nullptr || impl_->f3ddkr == nullptr) {
        return {};
    }
    const Rt64F3ddkrStats stats = impl_->f3ddkr->stats();
    // Assign by name: positional aggregate initialization silently
    // misattributes fields when either struct gains or reorders a member.
    Rt64GraphicsDiagnostics diagnostics{};
    diagnostics.matrix_commands = stats.matrix_commands;
    diagnostics.vertex_batches = stats.vertex_batches;
    diagnostics.vertices_loaded = stats.vertices_loaded;
    diagnostics.vertices_finite = stats.vertices_finite;
    diagnostics.vertices_in_clip = stats.vertices_in_clip;
    diagnostics.triangle_batches = stats.triangle_batches;
    diagnostics.triangles_drawn = stats.triangles_drawn;
    diagnostics.dma_display_lists = stats.dma_display_lists;
    diagnostics.color_image_address = stats.color_image_address;
    diagnostics.rejected_command_word0 = stats.rejected_command_word0;
    diagnostics.rejected_command_word1 = stats.rejected_command_word1;
    diagnostics.rejected_command_address = stats.rejected_command_address;
    diagnostics.rejection_reason = stats.rejection_reason;
    diagnostics.rejection_detail = stats.rejection_detail;
    diagnostics.rejection_source_address = stats.rejection_source_address;
    diagnostics.rejection_triangle = stats.rejection_triangle;
    diagnostics.loaded_vertices_low = stats.loaded_vertices_low;
    diagnostics.loaded_vertices_high = stats.loaded_vertices_high;
    diagnostics.full_sync_microseconds = stats.full_sync_microseconds;
    diagnostics.rdram_check_microseconds = impl_->last_rdram_check_microseconds;
    diagnostics.display_list_branches = stats.display_list_branches;
    diagnostics.last_display_list_address = stats.last_display_list_address;
    diagnostics.last_display_list_target = stats.last_display_list_target;
    return diagnostics;
}

Rt64FramebufferInfo Rt64Shell::last_framebuffer() const noexcept {
    return impl_ != nullptr ? impl_->last_framebuffer : Rt64FramebufferInfo{};
}

Rt64FrameView Rt64Shell::last_presented_frame() const noexcept {
    if (impl_ == nullptr || impl_->capture == nullptr) {
        return {};
    }
    const CaptureContext& capture = *impl_->capture;
    return {
        capture.width,
        capture.height,
        capture.row_pitch,
        capture.pixels,
    };
}

const char* rt64_shell_error_message(const Rt64ShellError error) noexcept {
    switch (error) {
    case Rt64ShellError::none:
        return "no error";
    case Rt64ShellError::invalid_window:
        return "RT64 requires a native window";
    case Rt64ShellError::invalid_memory:
        return "RT64 memory or VI registers are invalid";
    case Rt64ShellError::setup_dynamic_libraries_missing:
        return "RT64 dynamic libraries are missing";
    case Rt64ShellError::setup_invalid_graphics_api:
        return "RT64 graphics API is invalid";
    case Rt64ShellError::setup_graphics_api_not_found:
        return "RT64 graphics API is unavailable";
    case Rt64ShellError::setup_graphics_device_not_found:
        return "RT64 graphics device is unavailable";
    case Rt64ShellError::render_hook_unavailable:
        return "RT64 render hook is already owned by another shell";
    case Rt64ShellError::invalid_framebuffer:
        return "RT64 VI framebuffer address or dimensions are invalid";
    case Rt64ShellError::frame_capture_failed:
        return "RT64 presented-frame readback failed";
    case Rt64ShellError::invalid_task_address:
        return "RT64 task contains an out-of-range address";
    case Rt64ShellError::unsupported_microcode:
        return "RT64 does not recognize the submitted microcode";
    case Rt64ShellError::unsupported_commands:
        return "RT64 rejected an unsupported F3DDKR command";
    case Rt64ShellError::not_initialized:
        return "RT64 shell is not initialized";
    case Rt64ShellError::renderer_exception:
        return "RT64 raised an exception";
    case Rt64ShellError::conflicting_cpu_write:
        return "RT64 completion conflicts with CPU memory ownership";
    }
    return "unknown RT64 shell error";
}

}  // namespace jfg
