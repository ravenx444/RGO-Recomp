#pragma once

#include "jfg/boot/hle.hpp"

#include <cstddef>
#include <cstdint>

namespace jfg::boot {

// The game's runlink module records are 32-byte entries. Entry zero describes
// the resident program; overlay slot N is stored in entry N.
inline constexpr std::uint32_t kRunlinkModuleRecordSize = 32U;

struct RunlinkModuleIdentity final {
  std::uint32_t rom_start = 0U;
  std::uint32_t text_size = 0U;
  std::uint32_t data_size = 0U;
  std::uint32_t bss_size = 0U;
};

enum class RunlinkPublicationResult : std::uint8_t {
  published,
  already_published,
  table_uninitialized,
  invalid_argument,
  memory_fault,
  module_not_found,
  duplicate_module,
};

struct RunlinkAddressResolution final {
  RunlinkModuleIdentity identity{};
  std::uint32_t module_base = 0U;
  std::uint32_t text_offset = 0U;
};

enum class RunlinkAddressResult : std::uint8_t {
  resolved,
  table_uninitialized,
  invalid_argument,
  memory_fault,
  module_not_found,
  duplicate_module,
};

// Mirror a host-published synthetic overlay into the guest's runlink table.
// A real non-zero loader base always wins. The table pointer itself may still
// be zero during early boot; callers can retry when the overlay is dispatched.
[[nodiscard]] RunlinkPublicationResult publish_runlink_module(
    hle::GuestMemory &memory, std::uint32_t table_pointer_address,
    std::size_t overlay_slot_count, const RunlinkModuleIdentity &identity,
    std::uint32_t synthetic_base) noexcept;

// Resolve a call through the actual address chosen by the game's runlink
// allocator. Only executable text is accepted; data and BSS addresses must
// never become dispatch targets. Duplicate overlapping records fail closed.
[[nodiscard]] RunlinkAddressResult resolve_runlink_text_address(
    const hle::GuestMemory &memory, std::uint32_t table_pointer_address,
    std::size_t overlay_slot_count, std::uint32_t guest_address,
    RunlinkAddressResolution &resolution) noexcept;

// Suspended entries are (retained allocation base, module slot) pairs.
// Decide which guest loader to call without mutating retained state.
enum class RunlinkSuspensionResult : std::uint8_t { absent, suspended, invalid };
[[nodiscard]] RunlinkSuspensionResult resolve_runlink_suspension(
    const hle::GuestMemory& memory, std::uint32_t pending_table,
    std::size_t pending_count, std::uint32_t module_slot) noexcept;

// A full unload discards normalized mutable state. A suspended module still
// owns its data and BSS even though its executable base is zero.
enum class RunlinkRetentionResult : std::uint8_t { invalid, live, suspended, unloaded };
[[nodiscard]] RunlinkRetentionResult resolve_runlink_retention(
    const hle::GuestMemory& memory, std::uint32_t table_pointer_address,
    std::size_t overlay_slot_count, std::uint32_t pending_table,
    std::size_t pending_count, std::uint32_t module_slot) noexcept;

} // namespace jfg::boot
