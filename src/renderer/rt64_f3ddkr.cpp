#include "rt64_f3ddkr.hpp"
#include "jfg/renderer/rt64_f3ddkr_address.hpp"
#include "jfg/renderer/frame_interpolation.hpp"
#include "jfg/renderer/widescreen.hpp"

#include "common/rt64_math.h"
#include "gbi/rt64_gbi_f3d.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_application.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace jfg {
namespace {

constexpr std::uint8_t kOpDmaMatrix = 0x01U;
constexpr std::uint8_t kOpDmaTextureOffset = 0x02U;
constexpr std::uint8_t kOpDmaVertex = 0x04U;
constexpr std::uint8_t kOpDmaTriangles = 0x05U;
constexpr std::uint8_t kOpDisplayList = 0x06U;
constexpr std::uint8_t kOpDmaDisplayList = 0x07U;
constexpr std::uint8_t kOpSetPerspNorm = 0xB5U;
constexpr std::uint8_t kOpCullDisplayList = 0xBEU;
constexpr std::uint8_t kOpPopMatrix = 0xBDU;
constexpr std::uint8_t kOpMoveWord = 0xBCU;
constexpr std::uint8_t kOpDmaOffsets = 0xBFU;
constexpr std::uint8_t kOpSetTextureImage = 0xFDU;
constexpr std::uint8_t kOpSetDepthImage = 0xFEU;
constexpr std::uint8_t kOpSetColorImage = 0xFFU;
constexpr std::uint8_t kMoveWordBillboard = 0x02U;
constexpr std::uint8_t kMoveWordModelMatrix = 0x0AU;
constexpr std::size_t kMatrixCount = 32U;
constexpr std::size_t kVertexCount = 80U;
constexpr std::uint32_t kRdramBytes = 8U * 1024U * 1024U;
constexpr std::uint32_t kScratchAddress = kRdramBytes - 4096U;
// A guest list that branches to itself would otherwise spin forever on the
// game thread (jump form) or grow RT64's unbounded return stack (call form).
// The largest observed Goldwood task issues a few hundred G_DL edges; the
// budget is generous so only a cycle or a wild pointer can exhaust it.
constexpr std::size_t kMaximumDisplayListBranches = 65536U;
// The RSP microcode's own display-list stack is far shallower than this.
constexpr std::size_t kMaximumReturnStackDepth = 64U;
// F3D G_MOVEMEM payloads are at most 64 bytes (a full light set).
constexpr std::uint32_t kMaximumMoveMemBytes = 64U;

struct Context {
    RT64::Application* application = nullptr;
    RT64::GBI gbi{};
    std::array<hlslpp::float4x4, kMatrixCount> matrices{};
    std::array<std::uint32_t, kMatrixCount> matrix_segmented_addresses{};
    std::array<std::uint32_t, kMatrixCount> matrix_physical_addresses{};
    std::bitset<kMatrixCount> loaded_matrices{};
    std::uint32_t matrix_offset = 0U;
    std::uint32_t vertex_offset = 0U;
    std::uint32_t texture_offset = 0U;
    std::uint32_t texture_shift = 0U;
    std::size_t texture_count = 0U;
    std::size_t vertex_cursor = 0U;
    std::size_t selected_matrix = 0U;
    std::size_t commands = 0U;
    Rt64F3ddkrStats stats{};
    std::bitset<kVertexCount> loaded_vertices{};
    std::span<const std::uint8_t> simulation_rdram{};
    // RGO widescreen: RT64 reveals this much more of the scene horizontally.
    float horizontal_extent = 1.0F;
    bool billboard = false;
    bool failed = false;
    // RGO frame interpolation (docs/rgo/framerate.md). Off: every matrix is
    // a model matrix with an identity camera and is never interpolated.
    bool interpolate = false;
    bool interpolate_objects = false;
    const Rt64MatrixTags* matrix_tags = nullptr;
    std::uint32_t color_image_width = 0U;
    // The last slot-0 matrix could not be used as a camera (not invertible).
    bool camera_identity = false;
    // Per task: cameras seen so far, by [orthographic][screen target].
    std::array<std::array<std::uint32_t, 2>, 2> camera_ordinals{};
    // The interpolation group of each loaded slot, and the last tagged object
    // with how many bone matrices followed it.
    std::array<std::uint32_t, kMatrixCount> matrix_ids{};
    std::uint32_t object_id = 0U;
    std::uint32_t bone_index = 0U;
    // Across tasks: the two previous poses of every camera ID.
    std::unordered_map<std::uint32_t, std::pair<CameraPose, CameraPose>> camera_history;
};

std::mutex contexts_mutex;
std::unordered_map<RT64::State*, Context*> contexts;

[[nodiscard]] Context* find_context(RT64::State* state) noexcept {
    const std::scoped_lock lock(contexts_mutex);
    const auto iterator = contexts.find(state);
    return iterator == contexts.end() ? nullptr : iterator->second;
}

void reject(RT64::State* state, RT64::DisplayList** dl) noexcept {
    if (Context* context = find_context(state); context != nullptr) {
        context->failed = true;
        // A handler that already recorded a specific cause keeps it. Generic
        // rejections (an unsupported opcode reached by running past the end
        // of a list, for example) fall back to the most recent G_DL edge so
        // triage can distinguish a bad pointer from a list that ran on.
        if (context->stats.rejection_reason == 0U &&
            context->stats.rejection_source_address == 0U) {
            context->stats.rejection_source_address =
                context->stats.last_display_list_address;
            context->stats.rejection_detail =
                context->stats.last_display_list_target;
        }
        if (dl != nullptr && *dl != nullptr) {
            context->stats.rejected_command_word0 = (**dl).w0;
            context->stats.rejected_command_word1 = (**dl).w1;
            const std::uintptr_t cursor =
                reinterpret_cast<std::uintptr_t>(*dl);
            const std::array<std::uintptr_t, 2U> bases{
                state == nullptr
                    ? 0U
                    : reinterpret_cast<std::uintptr_t>(state->RDRAM),
                reinterpret_cast<std::uintptr_t>(
                    context->simulation_rdram.data()),
            };
            for (const std::uintptr_t base : bases) {
                if (base != 0U && cursor >= base &&
                    cursor - base <= kRdramBytes - sizeof(**dl)) {
                    context->stats.rejected_command_address =
                        static_cast<std::uint32_t>(cursor - base);
                    break;
                }
            }
        }
    }
    if (dl != nullptr) {
        *dl = nullptr;
    }
}

[[nodiscard]] bool physical_range(
    const std::uint32_t address,
    const std::size_t length) noexcept {
    return address <= kRdramBytes && length <= kRdramBytes - address;
}

[[nodiscard]] std::uint8_t guest_byte(
    const Context& context,
    const RT64::State& state,
    const std::uint32_t address) noexcept {
    if (context.simulation_rdram.size() == kRdramBytes) {
        return context.simulation_rdram[address ^ 3U];
    }
    return state.RDRAM[address ^ 3U];
}

[[nodiscard]] std::int16_t guest_s16(
    const Context& context,
    const RT64::State& state,
    const std::uint32_t address) noexcept {
    const std::uint16_t value =
        (static_cast<std::uint16_t>(guest_byte(context, state, address)) << 8U) |
        guest_byte(context, state, address + 1U);
    return static_cast<std::int16_t>(value);
}

[[nodiscard]] std::uint32_t segmented_physical(
    RT64::State& state,
    const std::uint32_t address) noexcept {
    return detail::resolve_f3ddkr_byte_source(
        state.rsp->fromSegmented(address));
}

RT64::TransformGroup ignored_group() noexcept {
    RT64::TransformGroup group;
    group.matrixId = G_EX_ID_IGNORE;
    group.decompose = false;
    group.positionInterpolation = G_EX_COMPONENT_SKIP;
    group.rotationInterpolation = G_EX_COMPONENT_SKIP;
    group.scaleInterpolation = G_EX_COMPONENT_SKIP;
    group.skewInterpolation = G_EX_COMPONENT_SKIP;
    group.perspectiveInterpolation = G_EX_COMPONENT_SKIP;
    group.tileInterpolation = G_EX_COMPONENT_SKIP;
    group.ordering = G_EX_ORDER_LINEAR;
    return group;
}

// RGO frame interpolation: a tagged object matrix. Decomposed, so rotating
// objects keep their shape; position AUTO snaps on velocity jumps
// (teleports, respawns, reused object slots).
RT64::TransformGroup object_group(const std::uint32_t id) noexcept {
    RT64::TransformGroup group;
    group.matrixId = id;
    group.decompose = true;
    group.positionInterpolation = G_EX_COMPONENT_AUTO;
    group.rotationInterpolation = G_EX_COMPONENT_INTERPOLATE;
    group.scaleInterpolation = G_EX_COMPONENT_INTERPOLATE;
    group.skewInterpolation = G_EX_COMPONENT_SKIP;
    group.perspectiveInterpolation = G_EX_COMPONENT_SKIP;
    group.vertexInterpolation = G_EX_COMPONENT_SKIP;
    group.texcoordInterpolation = G_EX_COMPONENT_SKIP;
    group.tileInterpolation = G_EX_COMPONENT_AUTO;
    group.ordering = G_EX_ORDER_LINEAR;
    return group;
}

// RGO frame interpolation: slot 0 is the camera (view x projection, or the
// HUD's ortho matrix). Give it to RT64 as the projection so RT64 can blend
// the camera between game frames; RT64 splits view and projection itself.
// RT64 divides every inserted MVP by the camera, so the camera must be
// cleanly invertible: finite, and camera x inverse close to the identity.
[[nodiscard]] bool invertible(const hlslpp::float4x4& camera) noexcept {
    const hlslpp::float4x4 inverse = hlslpp::inverse(camera);
    const hlslpp::float4x4 product = hlslpp::mul(camera, inverse);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            const float value = float(product[row][column]);
            const float expected = row == column ? 1.0F : 0.0F;
            if (!std::isfinite(float(inverse[row][column])) ||
                !(std::abs(value - expected) < 1e-3F))
                return false;
        }
    return true;
}

void apply_camera(Context& context) noexcept {
    RT64::RSP& rsp = *context.application->state->rsp;
    const int top = rsp.projectionMatrixStackSize - 1;
    if (!invertible(context.matrices[0U])) {
        // Keep the old handling for this slot: an identity camera, with the
        // matrix itself as each draw's MVP. Nothing here is blended.
        const hlslpp::float4x4 identity = hlslpp::float4x4::identity();
        rsp.viewMatrixStack[top] = identity;
        rsp.projMatrixStack[top] = identity;
        rsp.viewProjMatrixStack[top] = identity;
        rsp.projectionMatrixChanged = true;
        rsp.projectionMatrixInversed = false;
        auto& extended = rsp.extended;
        extended.viewProjMatrixIdStack[extended.viewProjMatrixIdStackSize - 1] = ignored_group();
        extended.viewProjMatrixIdStackChanged = true;
        context.camera_identity = true;
        return;
    }
    context.camera_identity = false;
    const hlslpp::float4x4& camera = context.matrices[0U];
    rsp.viewProjMatrixStack[top] = camera;
    if (RT64::isMatrixViewProj(camera)) {
        RT64::matrixDecomposeViewProj(camera, rsp.viewMatrixStack[top], rsp.projMatrixStack[top]);
    } else {
        rsp.projMatrixStack[top] = camera;
        rsp.viewMatrixStack[top] = hlslpp::float4x4::identity();
    }
    rsp.projectionMatrixSegmentedAddressStack[top] = context.matrix_segmented_addresses[0U];
    rsp.projectionMatrixPhysicalAddressStack[top] = context.matrix_physical_addresses[0U];
    rsp.projectionMatrixChanged = true;
    rsp.projectionMatrixInversed = false;

    const hlslpp::float4x4& projection = rsp.projMatrixStack[top];
    const bool orthographic = !(float(projection[3][3]) == 0.0F &&
                                std::abs(float(projection[1][1])) > 1e-6F);
    const bool screen = context.color_image_width >= 256U;
    std::uint32_t& ordinal =
        context.camera_ordinals[orthographic ? 1U : 0U][screen ? 1U : 0U];
    const std::uint32_t id = camera_group_id(orthographic, screen, ordinal++);
    bool cut = false;
    if (!orthographic) {
        std::array<std::array<float, 4>, 4> view{};
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                view[std::size_t(row)][std::size_t(column)] =
                    float(rsp.viewMatrixStack[top][row][column]);
        const CameraPose pose = camera_pose_from_view(view, float(projection[1][1]));
        auto& history = context.camera_history[id];
        cut = camera_cut(history.first, history.second, pose);
        history = {history.second, pose};
        if (context.camera_history.size() > 256U)
            context.camera_history.clear();
    }
    RT64::TransformGroup group;
    group.matrixId = id;
    group.decompose = false;  // cameras blend best in simple mode
    const std::uint8_t component = cut ? G_EX_COMPONENT_SKIP : G_EX_COMPONENT_INTERPOLATE;
    group.positionInterpolation = component;
    group.rotationInterpolation = component;
    group.scaleInterpolation = component;
    group.skewInterpolation = component;
    group.perspectiveInterpolation = component;
    group.vertexInterpolation = G_EX_COMPONENT_SKIP;
    group.texcoordInterpolation = G_EX_COMPONENT_SKIP;
    group.ordering = G_EX_ORDER_LINEAR;
    auto& extended = rsp.extended;
    extended.viewProjMatrixIdStack[extended.viewProjMatrixIdStackSize - 1] = group;
    extended.viewProjMatrixIdStackChanged = true;
}

void select_matrix(Context& context, const bool anchored = false) noexcept {
    RT64::RSP& rsp = *context.application->state->rsp;
    const std::size_t index = context.selected_matrix;
    rsp.modelMatrixStackSize = 1;
    rsp.modelMatrixSegmentedAddressStack[0] =
        context.matrix_segmented_addresses[index];
    rsp.modelMatrixPhysicalAddressStack[0] =
        context.matrix_physical_addresses[index];
    if (context.interpolate) {
        if (index == 0U && !anchored && context.loaded_matrices.test(0U) &&
            !context.camera_identity) {
            // World geometry: drawn with the camera alone.
            rsp.modelMatrixStack[0] = hlslpp::float4x4::identity();
            rsp.modelViewProjInserted = false;
            rsp.modelViewProjChanged = true;
            rsp.extended.modelMatrixIdStack[0] = ignored_group();
        } else {
            // A complete MVP: RT64 stores MVP x inverse(camera) as the world
            // matrix and still transforms vertices on the CPU with the MVP.
            rsp.modelMatrixStack[0] = context.matrices[index];
            rsp.modelViewProjMatrix = context.matrices[index];
            rsp.modelViewProjInserted = true;
            rsp.modelViewProjChanged = false;
            const std::uint32_t id = anchored ? 0U : context.matrix_ids[index];
            rsp.extended.modelMatrixIdStack[0] =
                id != 0U ? object_group(id) : ignored_group();
        }
        rsp.extended.modelMatrixIdStackChanged = true;
        return;
    }
    rsp.modelMatrixStack[0] = context.matrices[index];
    rsp.extended.modelMatrixIdStack[0] = ignored_group();
    rsp.extended.modelMatrixIdStackChanged = true;

    const int projection = rsp.projectionMatrixStackSize - 1;
    const hlslpp::float4x4 identity = hlslpp::float4x4::identity();
    rsp.viewMatrixStack[projection] = identity;
    rsp.projMatrixStack[projection] = identity;
    rsp.viewProjMatrixStack[projection] = identity;
    rsp.invViewProjMatrixStack[projection] = identity;
    rsp.projectionMatrixInversed = true;
    rsp.modelViewProjInserted = false;
    rsp.modelViewProjChanged = true;
}

void unsupported(RT64::State* state, RT64::DisplayList** dl) {
    reject(state, dl);
}

void no_op(RT64::State*, RT64::DisplayList**) {
}

void dma_matrix(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    ++context->stats.matrix_commands;
    const RT64::DisplayList command = **dl;
    const detail::F3ddkrMatrixCommand decoded_command =
        detail::decode_f3ddkr_matrix_command(command.w0);
    const std::size_t index = decoded_command.index;
    const bool multiply = decoded_command.multiply;
    const std::uint32_t segmented = state->rsp->fromSegmented(command.w1);
    if ((command.w0 & 0xFFFFU) != 64U ||
        index >= context->matrices.size()) {
        reject(state, dl);
        return;
    }
    const std::uint32_t address = detail::resolve_f3ddkr_dma_base(
        segmented, context->matrix_offset);
    if (!physical_range(address, sizeof(RT64::FixedMatrix))) {
        reject(state, dl);
        return;
    }
    const auto* fixed = reinterpret_cast<const RT64::FixedMatrix*>(
        context->simulation_rdram.size() == kRdramBytes
            ? context->simulation_rdram.data() + address
            : state->fromRDRAM(address));
    hlslpp::float4x4 decoded = fixed->toMatrix4x4();
    if (multiply) {
        if (!context->loaded_matrices.test(0U)) {
            reject(state, dl);
            return;
        }
        // JFG's multiply form composes the object matrix with slot zero's
        // already-combined camera MVP. RT64 uses row-vector matrices here.
        decoded = hlslpp::mul(decoded, context->matrices[0U]);
    }
    context->matrices[index] = decoded;
    context->loaded_matrices.set(index);
    context->matrix_segmented_addresses[index] = command.w1;
    context->matrix_physical_addresses[index] = address;
    context->selected_matrix = index;
    if (context->interpolate) {
        std::uint32_t id = 0U;
        if (context->interpolate_objects && index != 0U) {
            if (multiply) {
                // Bone matrices follow the object matrix loaded before them.
                id = bone_group_id(context->object_id, context->bone_index++);
            } else if (context->matrix_tags != nullptr) {
                std::array<std::uint32_t, 16> words{};
                const std::uint8_t* source = context->simulation_rdram.size() == kRdramBytes
                    ? context->simulation_rdram.data() + address
                    : state->fromRDRAM(address);
                std::memcpy(words.data(), source, sizeof(words));
                const auto tag = context->matrix_tags->find(address);
                if (tag != context->matrix_tags->end() &&
                    tag->second.first == matrix_words_hash(words))
                    id = tag->second.second;
                context->object_id = id;
                context->bone_index = 0U;
            }
        }
        context->matrix_ids[index] = id;
        if (index == 0U && !multiply)
            apply_camera(*context);
    }
    select_matrix(*context);
}

void dma_texture_offset(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    context->texture_offset = segmented_physical(*state, (*dl)->w1) &
        0x00FF'FFF8U;
    context->texture_shift = 0U;
    context->texture_count = 0U;
}

void set_color_image(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    const std::uint8_t format = static_cast<std::uint8_t>(
        (*dl)->p0(21U, 3U));
    const std::uint8_t size = static_cast<std::uint8_t>(
        (*dl)->p0(19U, 2U));
    const std::uint16_t width = static_cast<std::uint16_t>(
        (*dl)->p0(0U, 12U) + 1U);
    context->stats.color_image_address =
        segmented_physical(*state, (*dl)->w1);
    context->color_image_width = width;
    state->rdp->setColorImage(
        format, size, width, context->stats.color_image_address);
}

void set_depth_image(RT64::State* state, RT64::DisplayList** dl) {
    state->rdp->setDepthImage(segmented_physical(*state, (*dl)->w1));
}

void set_texture_image(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    const std::uint8_t format = static_cast<std::uint8_t>(
        (*dl)->p0(21U, 3U));
    const std::uint8_t size = static_cast<std::uint8_t>(
        (*dl)->p0(19U, 2U));
    const std::uint16_t width = static_cast<std::uint16_t>(
        (*dl)->p0(0U, 12U) + 1U);
    std::uint32_t address = segmented_physical(*state, (*dl)->w1);
    if (context->texture_offset != 0U) {
        if (format == G_IM_FMT_RGBA) {
            const std::uint32_t shift_address = context->texture_offset +
                static_cast<std::uint32_t>(context->texture_count * 2U);
            if (!physical_range(shift_address, 2U)) {
                reject(state, dl);
                return;
            }
            context->texture_shift = static_cast<std::uint16_t>(
                guest_s16(*context, *state, shift_address));
            address = detail::resolve_f3ddkr_byte_source(
                address + context->texture_shift);
        }
        else {
            context->texture_offset = 0U;
            context->texture_shift = 0U;
            context->texture_count = 0U;
        }
    }
    state->rdp->setTextureImage(format, size, width, address);
}

void load_block(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    const std::uint16_t lrs = static_cast<std::uint16_t>(
        (*dl)->p1(12U, 12U));
    if (context->texture_offset != 0U) {
        const std::uint32_t block_bytes = ((lrs >> 2U) + 1U) << 3U;
        if (block_bytes != 0U &&
            (context->texture_shift % block_bytes) != 0U) {
            // The shift is a raw guest s16. Undoing a shift larger than the
            // current texture address would wrap to a ~4 GiB address that
            // RT64's loadBlock dereferences unchecked.
            if (context->texture_shift > state->rdp->texture.address) {
                context->stats.rejection_reason = kF3ddkrRejectTextureShiftWrap;
                context->stats.rejection_detail = context->texture_shift;
                reject(state, dl);
                return;
            }
            state->rdp->texture.address -= context->texture_shift;
            context->texture_offset = 0U;
            context->texture_shift = 0U;
            context->texture_count = 0U;
        }
        else {
            ++context->texture_count;
        }
    }
    RT64::GBI_RDP::loadBlock(state, dl);
}

void dma_vertex(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    ++context->stats.vertex_batches;
    const RT64::DisplayList command = **dl;
    const std::size_t count = (command.w0 >> 19U) & 0x1FU;
    const std::size_t requested_start = (command.w0 >> 9U) & 0x1FU;
    const bool append = (command.w0 & 0x0001'0000U) != 0U;
    if (append) {
        if (context->billboard) {
            context->vertex_cursor = 1U;
        }
    }
    else {
        context->vertex_cursor = 0U;
    }
    if (requested_start > kVertexCount - context->vertex_cursor) {
        reject(state, dl);
        return;
    }
    const std::size_t first = context->vertex_cursor + requested_start;
    const std::uint32_t segmented = state->rsp->fromSegmented(command.w1);
    if (count == 0U || first > kVertexCount || count > kVertexCount - first) {
        reject(state, dl);
        return;
    }
    const std::uint32_t address = detail::resolve_f3ddkr_vertex_dma_source(
        segmented, context->vertex_offset, command.w0);
    const std::size_t source_bytes = count * 10U;
    if (!physical_range(address, source_bytes)) {
        reject(state, dl);
        return;
    }

    hlslpp::float4x4 unanchored_matrix{};
    bool anchored_billboard = false;
    if (context->billboard) {
        const int workload_cursor = state->ext.workloadQueue->writeCursor;
        RT64::Workload& workload =
            state->ext.workloadQueue->workloads[workload_cursor];
        const std::uint32_t anchor_index = state->rsp->indices[0U];
        if (anchor_index >= workload.drawData.posTransformed.size()) {
            reject(state, dl);
            return;
        }
        unanchored_matrix = context->matrices[context->selected_matrix];
        context->matrices[context->selected_matrix][3] +=
            workload.drawData.posTransformed[anchor_index];
        anchored_billboard = true;
    }
    select_matrix(*context, anchored_billboard);
    std::array<std::byte, sizeof(RT64::RSP::Vertex) * 32U> saved{};
    auto* scratch = state->fromRDRAM(kScratchAddress);
    const std::size_t scratch_bytes = count * sizeof(RT64::RSP::Vertex);
    for (std::size_t index = 0U; index < scratch_bytes; ++index) {
        saved[index] = static_cast<std::byte>(scratch[index]);
    }
    auto* vertices = reinterpret_cast<RT64::RSP::Vertex*>(scratch);
    for (std::size_t index = 0U; index < count; ++index) {
        const std::uint32_t source =
            address + static_cast<std::uint32_t>(index * 10U);
        RT64::RSP::Vertex& vertex = vertices[index];
        vertex.x = guest_s16(*context, *state, source);
        vertex.y = guest_s16(*context, *state, source + 2U);
        vertex.z = guest_s16(*context, *state, source + 4U);
        vertex.flag = 0U;
        vertex.s = 0;
        vertex.t = 0;
        vertex.color.r = guest_byte(*context, *state, source + 6U);
        vertex.color.g = guest_byte(*context, *state, source + 7U);
        vertex.color.b = guest_byte(*context, *state, source + 8U);
        vertex.color.a = guest_byte(*context, *state, source + 9U);
    }
    state->rsp->setVertex(
        kScratchAddress,
        static_cast<std::uint32_t>(count),
        static_cast<std::uint32_t>(first));
    {
        const int workload_cursor = state->ext.workloadQueue->writeCursor;
        RT64::Workload& workload =
            state->ext.workloadQueue->workloads[workload_cursor];
        for (std::size_t index = first; index < first + count; ++index) {
            const std::uint32_t global = state->rsp->indices[index];
            if (global >= workload.drawData.posTransformed.size()) {
                continue;
            }
            const hlslpp::float4 position =
                workload.drawData.posTransformed[global];
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z) || !std::isfinite(position.w)) {
                continue;
            }
            ++context->stats.vertices_finite;
            const float limit = std::abs(position.w) * 2.0F;
            if (position.w != 0.0F && std::abs(position.x) <= limit &&
                std::abs(position.y) <= limit &&
                std::abs(position.z) <= limit) {
                ++context->stats.vertices_in_clip;
            }
        }
    }
    for (std::size_t index = 0U; index < scratch_bytes; ++index) {
        scratch[index] = std::to_integer<std::uint8_t>(saved[index]);
    }
    if (anchored_billboard) {
        context->matrices[context->selected_matrix] = unanchored_matrix;
        select_matrix(*context);
    }
    for (std::size_t index = first; index < first + count; ++index) {
        context->loaded_vertices.set(index);
    }
    context->stats.vertices_loaded += count;
    context->vertex_cursor += count;
}

[[nodiscard]] bool set_vertex_texture_coordinates(
    RT64::State& state,
    RT64::RSP& rsp,
    const std::uint8_t vertex,
    const std::uint32_t coordinate) {
    const int workload_cursor = state.ext.workloadQueue->writeCursor;
    RT64::Workload& workload =
        state.ext.workloadQueue->workloads[workload_cursor];
    auto& draw = workload.drawData;
    std::uint32_t global = rsp.indices[vertex];
    const auto has_components = [global](const std::size_t size,
                                         const std::size_t stride) {
        return stride != 0U && global <= (SIZE_MAX - (stride - 1U)) / stride &&
            static_cast<std::size_t>(global) * stride + (stride - 1U) < size;
    };
    if (!has_components(draw.posFloats.size(), 3U) ||
        !has_components(draw.velFloats.size(), 3U) ||
        !has_components(draw.normColBytes.size(), 4U) ||
        !has_components(draw.tcFloats.size(), 2U) ||
        !has_components(draw.tcVelFloats.size(), 2U) ||
        global >= draw.viewProjIndices.size() ||
        global >= draw.worldIndices.size() || global >= draw.fogIndices.size() ||
        global >= draw.lightIndices.size() || global >= draw.lightCounts.size() ||
        global >= draw.lookAtIndices.size() ||
        global >= draw.posTransformed.size() || global >= draw.posScreen.size()) {
        return false;
    }

    // RT64's pinned modifyVertex implementation appends directly from its
    // own vectors. A growth reallocation can invalidate the source reference,
    // corrupting positions when F3DDKR clones a shared vertex for per-triangle
    // texture coordinates. Clone through local values so this adapter remains
    // correct without modifying the pinned dependency checkout.
    if (rsp.used[vertex]) {
        const std::uint32_t replacement = draw.vertexCount();
        const float px = draw.posFloats[global * 3U];
        const float py = draw.posFloats[global * 3U + 1U];
        const float pz = draw.posFloats[global * 3U + 2U];
        const float vx = draw.velFloats[global * 3U];
        const float vy = draw.velFloats[global * 3U + 1U];
        const float vz = draw.velFloats[global * 3U + 2U];
        const std::uint8_t red = draw.normColBytes[global * 4U];
        const std::uint8_t green = draw.normColBytes[global * 4U + 1U];
        const std::uint8_t blue = draw.normColBytes[global * 4U + 2U];
        const std::uint8_t alpha = draw.normColBytes[global * 4U + 3U];
        const float texture_s = draw.tcFloats[global * 2U];
        const float texture_t = draw.tcFloats[global * 2U + 1U];
        const float velocity_s = draw.tcVelFloats[global * 2U];
        const float velocity_t = draw.tcVelFloats[global * 2U + 1U];
        const auto view_projection = draw.viewProjIndices[global];
        const auto world = draw.worldIndices[global];
        const auto fog = draw.fogIndices[global];
        const auto light = draw.lightIndices[global];
        const auto light_count = draw.lightCounts[global];
        const auto look_at = draw.lookAtIndices[global];
        const auto transformed = draw.posTransformed[global];
        const auto screen = draw.posScreen[global];

        draw.posFloats.insert(draw.posFloats.end(), {px, py, pz});
        draw.velFloats.insert(draw.velFloats.end(), {vx, vy, vz});
        draw.normColBytes.insert(
            draw.normColBytes.end(), {red, green, blue, alpha});
        draw.tcFloats.insert(draw.tcFloats.end(), {texture_s, texture_t});
        draw.tcVelFloats.insert(
            draw.tcVelFloats.end(), {velocity_s, velocity_t});
        draw.viewProjIndices.emplace_back(view_projection);
        draw.worldIndices.emplace_back(world);
        draw.fogIndices.emplace_back(fog);
        draw.lightIndices.emplace_back(light);
        draw.lightCounts.emplace_back(light_count);
        draw.lookAtIndices.emplace_back(look_at);
        draw.posTransformed.emplace_back(transformed);
        draw.posScreen.emplace_back(screen);
        rsp.indices[vertex] = replacement;
        rsp.used[vertex] = false;
        global = replacement;
    }

    draw.tcFloats[global * 2U] =
        static_cast<float>(static_cast<std::int16_t>(coordinate >> 16U)) /
        32.0F;
    draw.tcFloats[global * 2U + 1U] =
        static_cast<float>(static_cast<std::int16_t>(coordinate)) / 32.0F;
    draw.lookAtIndices[global] = 0U;
    return true;
}

[[nodiscard]] std::uint8_t vertex_clip_code(
    const hlslpp::float4& position, const float horizontal_extent) noexcept {
    return widescreen_clip_code(
        position.x, position.y, position.w, horizontal_extent);
}

void capture_loaded_vertices(Context& context) noexcept {
    context.stats.loaded_vertices_low = 0U;
    context.stats.loaded_vertices_high = 0U;
    for (std::size_t index = 0U; index < 64U; ++index) {
        if (context.loaded_vertices.test(index))
            context.stats.loaded_vertices_low |= std::uint64_t{1U} << index;
    }
    for (std::size_t index = 64U;
         index < context.loaded_vertices.size(); ++index) {
        if (context.loaded_vertices.test(index))
            context.stats.loaded_vertices_high |=
                static_cast<std::uint16_t>(1U << (index - 64U));
    }
}

void dma_triangles(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    ++context->stats.triangle_batches;
    const RT64::DisplayList command = **dl;
    const std::size_t count = ((command.w0 >> 20U) & 0x0FU) + 1U;
    const std::uint8_t texture =
        static_cast<std::uint8_t>((command.w0 >> 16U) & 0x0FU);
    const std::uint32_t address = detail::resolve_f3ddkr_direct_dma_source(
        state->rsp->fromSegmented(command.w1));
    if (!physical_range(address, count * 16U)) {
        context->stats.rejection_reason = 1U;
        context->stats.rejection_detail = address;
        reject(state, dl);
        return;
    }
    // The F3DDKR command changes only gSP.texture.on. RT64 stores the mip
    // count as level + 1, so feeding textureState.levels back through
    // setTexture() would incorrectly increment an unrelated field on every
    // DMA triangle batch.
    state->rsp->textureState.on = texture;
    for (std::size_t triangle = 0U; triangle < count; ++triangle) {
        const std::uint32_t source =
            address + static_cast<std::uint32_t>(triangle * 16U);
        const std::uint8_t flags = guest_byte(*context, *state, source);
        const std::array<std::uint8_t, 3U> vertices{
            guest_byte(*context, *state, source + 1U),
            guest_byte(*context, *state, source + 2U),
            guest_byte(*context, *state, source + 3U),
        };
        for (const std::uint8_t vertex : vertices) {
            if (vertex >= context->loaded_vertices.size() ||
                !context->loaded_vertices.test(vertex)) {
                context->stats.rejection_reason = 2U;
                context->stats.rejection_detail = vertex;
                context->stats.rejection_source_address = source;
                context->stats.rejection_triangle =
                    static_cast<std::uint32_t>(triangle);
                capture_loaded_vertices(*context);
                reject(state, dl);
                return;
            }
        }
        const int workload_cursor = state->ext.workloadQueue->writeCursor;
        const RT64::Workload& workload =
            state->ext.workloadQueue->workloads[workload_cursor];
        std::uint8_t common_clip = 0x1FU;
        for (const std::uint8_t vertex : vertices) {
            const std::uint32_t global = state->rsp->indices[vertex];
            if (global >= workload.drawData.posTransformed.size()) {
                context->stats.rejection_reason = 3U;
                context->stats.rejection_detail = global;
                reject(state, dl);
                return;
            }
            common_clip &=
                vertex_clip_code(workload.drawData.posTransformed[global],
                    context->horizontal_extent);
        }
        if (common_clip != 0U)
            continue;
        const std::uint32_t cull_mode = (flags & 0x40U) != 0U
            ? 0U
            : (state->rsp->viewportStack[
                    state->rsp->viewportStackSize - 1U].scale.x > 0.0F
                ? state->rsp->cullBothMask ^ state->rsp->cullFrontMask
                : state->rsp->cullFrontMask);
        state->rsp->modifyGeometryMode(
            ~state->rsp->cullBothMask, cull_mode);
        for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
            const std::uint32_t coordinate =
                (static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                    guest_s16(*context, *state, source + 4U +
                        static_cast<std::uint32_t>(corner * 4U)))) << 16U) |
                static_cast<std::uint16_t>(guest_s16(
                    *context, *state, source + 6U +
                        static_cast<std::uint32_t>(corner * 4U)));
            if (!set_vertex_texture_coordinates(
                    *state, *state->rsp, vertices[corner], coordinate)) {
                context->stats.rejection_reason = 4U;
                context->stats.rejection_detail = vertices[corner];
                reject(state, dl);
                return;
            }
        }
        state->rsp->drawIndexedTri(vertices[0U], vertices[1U], vertices[2U]);
        ++context->stats.triangles_drawn;
    }
    context->vertex_cursor = 0U;
}

void set_perspective_normalization(
    RT64::State* state,
    RT64::DisplayList** dl) {
    if (Context* context = find_context(state); context != nullptr) {
        ++context->commands;
    }
    state->rsp->setPerspNorm((*dl)->p1(16U, 16U));
}

void cull_display_list(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr || dl == nullptr || *dl == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    std::size_t first = ((*dl)->w0 & 0x00FF'FFFFU) / 40U;
    if ((*dl)->w1 < 40U) {
        reject(state, dl);
        return;
    }
    std::size_t last = ((*dl)->w1 / 40U) - 1U;
    if (last < first)
        std::swap(first, last);
    if (last >= context->loaded_vertices.size()) {
        reject(state, dl);
        return;
    }

    const int workload_cursor = state->ext.workloadQueue->writeCursor;
    const RT64::Workload& workload =
        state->ext.workloadQueue->workloads[workload_cursor];
    std::uint8_t common_clip = 0x1FU;
    for (std::size_t vertex = first; vertex <= last; ++vertex) {
        if (!context->loaded_vertices.test(vertex)) {
            reject(state, dl);
            return;
        }
        const std::uint32_t global = state->rsp->indices[vertex];
        if (global >= workload.drawData.posTransformed.size()) {
            reject(state, dl);
            return;
        }
        const hlslpp::float4 position =
            workload.drawData.posTransformed[global];
        common_clip &=
            vertex_clip_code(position, context->horizontal_extent);
        if (common_clip == 0U)
            return;
    }
    // A cull at the top level has nothing to return to. RT64 would report
    // that as a clean EndDL and silently terminate the whole task.
    if (state->returnAddressStack.empty()) {
        context->stats.rejection_reason = kF3ddkrRejectReturnStackUnderflow;
        reject(state, dl);
        return;
    }
    *dl = state->popReturnAddress();
}

void move_mem(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    // Stock F3D moveMem dereferences a 24-bit masked address into RT64's
    // 8 MiB snapshot without a range check. Bound it here before delegating.
    const std::uint32_t address = state->rsp->fromSegmentedMasked((*dl)->w1);
    if (!physical_range(address, kMaximumMoveMemBytes)) {
        context->stats.rejection_reason = kF3ddkrRejectMoveMemRange;
        context->stats.rejection_detail = address;
        reject(state, dl);
        return;
    }
    RT64::GBI_F3D::moveMem(state, dl);
}

void move_word(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    const std::uint8_t subtype = static_cast<std::uint8_t>((*dl)->w0);
    if (subtype == kMoveWordBillboard) {
        if (((*dl)->w1 & ~1U) != 0U) {
            reject(state, dl);
            return;
        }
        context->billboard = ((*dl)->w1 & 1U) != 0U;
        return;
    }
    if (subtype == kMoveWordModelMatrix) {
        context->selected_matrix = ((*dl)->w1 >> 6U) & 0x03U;
        select_matrix(*context);
        return;
    }
    RT64::GBI_F3D::moveWord(state, dl);
}

void dma_offsets(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    context->matrix_offset = (*dl)->w0 & 0x00FF'FFFFU;
    context->vertex_offset = (*dl)->w1 & 0x00FF'FFFFU;
    // The original BF handler clears the texture-offset enable byte (DMEM
    // 0x1E7). A new model's DMA bases must not retain the prior texture table.
    context->texture_offset = 0U;
    context->texture_shift = 0U;
    context->texture_count = 0U;
}

void run_display_list(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr || context->simulation_rdram.size() != kRdramBytes) {
        reject(state, dl);
        return;
    }
    ++context->stats.display_list_branches;
    if (context->stats.display_list_branches > kMaximumDisplayListBranches) {
        context->stats.rejection_reason = kF3ddkrRejectBranchBudget;
        context->stats.rejection_detail = static_cast<std::uint32_t>(
            context->stats.display_list_branches);
        reject(state, dl);
        return;
    }
    if ((*dl)->p0(16U, 1U) == 0U) {
        if (state->returnAddressStack.size() >= kMaximumReturnStackDepth) {
            context->stats.rejection_reason = kF3ddkrRejectReturnStackDepth;
            context->stats.rejection_detail = static_cast<std::uint32_t>(
                state->returnAddressStack.size());
            reject(state, dl);
            return;
        }
        state->pushReturnAddress(*dl);
    }
    const std::uint32_t address = detail::resolve_f3ddkr_direct_dma_source(
        state->rsp->fromSegmented((*dl)->w1));
    const std::uintptr_t command_cursor =
        reinterpret_cast<std::uintptr_t>(*dl);
    const std::uintptr_t simulation_base = reinterpret_cast<std::uintptr_t>(
        context->simulation_rdram.data());
    if (command_cursor >= simulation_base &&
        command_cursor - simulation_base <=
            kRdramBytes - sizeof(RT64::DisplayList)) {
        // Remember the most recent direct display-list edge. reject() copies
        // it into the rejection fields only when no handler recorded a more
        // specific cause, so a later successful edge cannot clobber an
        // earlier triangle diagnostic.
        context->stats.last_display_list_address =
            static_cast<std::uint32_t>(command_cursor - simulation_base);
        context->stats.last_display_list_target = address;
    }
    if (!physical_range(address, sizeof(RT64::DisplayList))) {
        reject(state, dl);
        return;
    }
    *dl = reinterpret_cast<RT64::DisplayList*>(
        const_cast<std::uint8_t*>(context->simulation_rdram.data()) +
        address) - 1;
}

void full_sync(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    const auto start = std::chrono::steady_clock::now();
    RT64::GBI_RDP::fullSync(state, dl);
    if (context != nullptr) {
        context->stats.full_sync_microseconds +=
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());
    }
}

void dispatch_nested(RT64::State* state, RT64::DisplayList*& command) {
    Context* context = find_context(state);
    if (context == nullptr || command == nullptr) {
        return;
    }
    const std::uint8_t opcode = static_cast<std::uint8_t>(command->w0 >> 24U);
    if ((opcode & 0xC0U) != 0xC0U) {
        // F3DDKR's counted DMA-list handler dispatches only RDP commands;
        // low opcodes inside the fixed-size block are data words, not
        // commands. Count them so a block that draws nothing is visible in
        // diagnostics without rejecting valid guest data.
        ++context->stats.nested_data_words;
        return;
    }
    const RT64::GBIFunction function = context->gbi.map[opcode];
    if (function == nullptr) {
        reject(state, &command);
        return;
    }
    function(state, &command);
}

void dma_display_list(RT64::State* state, RT64::DisplayList** dl) {
    Context* context = find_context(state);
    if (context == nullptr) {
        reject(state, dl);
        return;
    }
    ++context->commands;
    ++context->stats.dma_display_lists;
    const RT64::DisplayList outer = **dl;
    const std::size_t count = (outer.w0 >> 16U) & 0xFFU;
    const std::size_t encoded_bytes = outer.w0 & 0xFFFFU;
    const std::uint32_t address = detail::resolve_f3ddkr_direct_dma_source(
        state->rsp->fromSegmented(outer.w1));
    if (count == 0U || encoded_bytes != count * sizeof(RT64::DisplayList) ||
        !physical_range(address, encoded_bytes)) {
        reject(state, dl);
        return;
    }
    RT64::DisplayList* nested = reinterpret_cast<RT64::DisplayList*>(
        context->simulation_rdram.size() == kRdramBytes
            ? const_cast<std::uint8_t*>(context->simulation_rdram.data()) +
                address
            : state->fromRDRAM(address));
    RT64::DisplayList* const end = nested + count;
    while (nested != nullptr && nested < end && !context->failed) {
        RT64::DisplayList* current = nested;
        dispatch_nested(state, nested);
        if (nested == nullptr) {
            // EndDL is valid only as the final encoded word of a counted
            // list. It must not silently truncate the block.
            if (current + 1 != end) {
                reject(state, dl);
            }
            break;
        }
        RT64::DisplayList* const next = nested + 1;
        if (next <= current || next > end) {
            // Handlers such as TextureRectangle advance over continuation
            // words themselves. Count those words, and reject control flow
            // that escapes the explicitly bounded block.
            reject(state, dl);
            return;
        }
        nested = next;
    }
}

void reset(RT64::State* state) {
    RT64::GBI_F3D::reset(state);
    if (Context* context = find_context(state); context != nullptr) {
        context->matrix_offset = 0U;
        context->vertex_offset = 0U;
        context->texture_offset = 0U;
        context->texture_shift = 0U;
        context->texture_count = 0U;
        context->vertex_cursor = 0U;
        context->loaded_vertices.reset();
        context->selected_matrix = 0U;
        context->matrix_segmented_addresses.fill(0U);
        context->matrix_physical_addresses.fill(0U);
        context->loaded_matrices.reset();
        context->billboard = false;
        context->camera_ordinals = {};
        context->matrix_ids.fill(0U);
        context->object_id = 0U;
        context->bone_index = 0U;
        context->color_image_width = 0U;
        context->camera_identity = false;
    }
}

}  // namespace

struct Rt64F3ddkr::Impl {
    explicit Impl(RT64::Application& app) {
        context.application = &app;
    }
    Context context;
};

Rt64F3ddkr::Rt64F3ddkr(RT64::Application& application)
    : impl_(std::make_unique<Impl>(application)) {
    const std::scoped_lock lock(contexts_mutex);
    contexts.emplace(application.state.get(), &impl_->context);
}

Rt64F3ddkr::~Rt64F3ddkr() {
    if (impl_ != nullptr && impl_->context.application != nullptr &&
        impl_->context.application->state != nullptr) {
        const std::scoped_lock lock(contexts_mutex);
        contexts.erase(impl_->context.application->state.get());
    }
}

void Rt64F3ddkr::begin(
    const std::span<const std::uint8_t> simulation_rdram) noexcept {
    impl_->context.simulation_rdram = simulation_rdram;
    impl_->context.commands = 0U;
    impl_->context.stats = {};
    impl_->context.failed = false;
    reset(impl_->context.application->state.get());
}

void Rt64F3ddkr::install() noexcept {
    Context& context = impl_->context;
    context.gbi = {};
    context.gbi.ucode = RT64::GBIUCode::F3D;
    RT64::GBI_RDP::setup(&context.gbi, true);
    RT64::GBI_F3D::setup(&context.gbi);
    for (RT64::GBIFunction& function : context.gbi.map) {
        if (function == nullptr) {
            function = &unsupported;
        }
    }
    context.gbi.map[kOpDmaMatrix] = &dma_matrix;
    context.gbi.map[kOpDmaTextureOffset] = &dma_texture_offset;
    context.gbi.map[kOpDmaVertex] = &dma_vertex;
    context.gbi.map[kOpDmaTriangles] = &dma_triangles;
    context.gbi.map[kOpDisplayList] = &run_display_list;
    context.gbi.map[kOpDmaDisplayList] = &dma_display_list;
    context.gbi.map[G_RDPFULLSYNC] = &full_sync;
    context.gbi.map[kOpSetPerspNorm] = &set_perspective_normalization;
    context.gbi.map[kOpCullDisplayList] = &cull_display_list;
    context.gbi.map[kOpPopMatrix] = &no_op;
    context.gbi.map[kOpMoveWord] = &move_word;
    context.gbi.map[kOpDmaOffsets] = &dma_offsets;
    context.gbi.map[kOpSetTextureImage] = &set_texture_image;
    context.gbi.map[kOpSetDepthImage] = &set_depth_image;
    context.gbi.map[kOpSetColorImage] = &set_color_image;
    context.gbi.map[G_LOADBLOCK] = &load_block;
    context.gbi.map[F3D_G_MOVEMEM] = &move_mem;
    // F3DDKR has no sprite microcode; stock F3D would interpret 0x09 as a
    // Sprite2D base pointer and dereference it unchecked.
    context.gbi.map[F3D_G_SPRITE2D_BASE] = &unsupported;
    context.gbi.resetFromTask = &reset;
    RT64::Application& application = *context.application;
    application.interpreter->hleGBI = &context.gbi;
    application.state->rsp->setGBI(&context.gbi);
}

void Rt64F3ddkr::set_interpolation(const bool enabled, const bool objects,
                                   const Rt64MatrixTags* tags) noexcept {
    impl_->context.interpolate = enabled;
    impl_->context.interpolate_objects = enabled && objects;
    impl_->context.matrix_tags = tags;
}

void Rt64F3ddkr::set_horizontal_extent(const float extent) noexcept {
    impl_->context.horizontal_extent =
        std::isfinite(extent) && extent > 1.0F ? extent : 1.0F;
}

bool Rt64F3ddkr::complete() const noexcept {
    // A valid setup list may contain only stock RDP/GBI commands (for
    // example FullSync followed by EndDL), so custom-command count is a
    // diagnostic rather than an acceptance requirement.
    return impl_ != nullptr && !impl_->context.failed;
}

std::size_t Rt64F3ddkr::command_count() const noexcept {
    return impl_ == nullptr ? 0U : impl_->context.commands;
}

Rt64F3ddkrStats Rt64F3ddkr::stats() const noexcept {
    return impl_ == nullptr ? Rt64F3ddkrStats{} : impl_->context.stats;
}

}  // namespace jfg
