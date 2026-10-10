// RT64 shell memory helpers that need no renderer: RDRAM snapshot copies,
// merges and commits, and VI framebuffer inspection. Shared by the RT64
// shell and the headless Linux build (rt64_shell_headless.cpp).
#include "jfg/runtime/snapshot_copy.hpp"
#include "jfg/renderer/rt64_shell.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace jfg {

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
