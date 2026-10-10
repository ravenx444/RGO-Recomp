#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

namespace RT64 {
struct Application;
}

namespace jfg {

struct Rt64F3ddkrStats {
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
    std::size_t display_list_branches = 0U;
    std::uint32_t last_display_list_address = 0U;
    std::uint32_t last_display_list_target = 0U;
    std::size_t nested_data_words = 0U;
    // RGO frame diagnostics: hashes of every matrix this task loaded, and of
    // its camera (slot 0) matrices alone (jfg/renderer/frame_stats.hpp).
    std::uint64_t scene_hash = 0xCBF29CE484222325ULL;
    std::uint64_t camera_hash = 0xCBF29CE484222325ULL;
};

// Rejection reason codes recorded in Rt64F3ddkrStats::rejection_reason.
inline constexpr std::uint32_t kF3ddkrRejectTriangleSource = 1U;
inline constexpr std::uint32_t kF3ddkrRejectTriangleVertexUnloaded = 2U;
inline constexpr std::uint32_t kF3ddkrRejectTriangleVertexIndex = 3U;
inline constexpr std::uint32_t kF3ddkrRejectTriangleTexcoord = 4U;
inline constexpr std::uint32_t kF3ddkrRejectBranchBudget = 5U;
inline constexpr std::uint32_t kF3ddkrRejectReturnStackDepth = 6U;
inline constexpr std::uint32_t kF3ddkrRejectReturnStackUnderflow = 7U;
inline constexpr std::uint32_t kF3ddkrRejectTextureShiftWrap = 9U;
inline constexpr std::uint32_t kF3ddkrRejectMoveMemRange = 10U;

// RGO frame interpolation: object matrix tags recorded by the game hooks,
// by physical matrix address: {hash of the matrix words, interpolation ID}.
using Rt64MatrixTags =
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>;

class Rt64F3ddkr final {
public:
    explicit Rt64F3ddkr(RT64::Application& application);
    ~Rt64F3ddkr();
    Rt64F3ddkr(const Rt64F3ddkr&) = delete;
    Rt64F3ddkr& operator=(const Rt64F3ddkr&) = delete;

    void begin(std::span<const std::uint8_t> simulation_rdram) noexcept;
    void install() noexcept;
    // Accept geometry up to extent * w horizontally (RGO widescreen).
    void set_horizontal_extent(float extent) noexcept;
    // RGO frame interpolation: give RT64 the camera and matrix IDs so it can
    // blend frames. Off (the default) keeps the original matrix handling.
    void set_interpolation(bool enabled, bool objects,
                           const Rt64MatrixTags* tags) noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::size_t command_count() const noexcept;
    [[nodiscard]] Rt64F3ddkrStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jfg
