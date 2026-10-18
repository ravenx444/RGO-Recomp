#include "jfg/boot/runlink_module_table.hpp"

#include <limits>

namespace jfg::boot {
namespace {

constexpr std::uint32_t kBaseOffset = 0U;
constexpr std::uint32_t kRomStartOffset = 4U;
constexpr std::uint32_t kTextSizeOffset = 8U;
constexpr std::uint32_t kDataSizeOffset = 12U;
constexpr std::uint32_t kBssSizeOffset = 16U;

} // namespace

RunlinkAddressResult resolve_runlink_text_address(
    const hle::GuestMemory &memory,
    const std::uint32_t table_pointer_address,
    const std::size_t overlay_slot_count, const std::uint32_t guest_address,
    RunlinkAddressResolution &resolution) noexcept {
  resolution = {};
  if (overlay_slot_count == 0U || guest_address == 0U ||
      overlay_slot_count >
          (std::numeric_limits<std::uint32_t>::max() /
           kRunlinkModuleRecordSize) - 1U)
    return RunlinkAddressResult::invalid_argument;

  std::uint32_t table = 0U;
  if (!memory.read_u32(table_pointer_address, table))
    return RunlinkAddressResult::memory_fault;
  if (table == 0U)
    return RunlinkAddressResult::table_uninitialized;

  bool matched = false;
  for (std::size_t slot = 1U; slot <= overlay_slot_count; ++slot) {
    const std::uint32_t record =
        table + static_cast<std::uint32_t>(slot) * kRunlinkModuleRecordSize;
    if (record < table)
      return RunlinkAddressResult::memory_fault;
    RunlinkAddressResolution candidate{};
    if (!memory.read_u32(record + kBaseOffset, candidate.module_base) ||
        !memory.read_u32(record + kRomStartOffset,
                         candidate.identity.rom_start) ||
        !memory.read_u32(record + kTextSizeOffset,
                         candidate.identity.text_size) ||
        !memory.read_u32(record + kDataSizeOffset,
                         candidate.identity.data_size) ||
        !memory.read_u32(record + kBssSizeOffset,
                         candidate.identity.bss_size))
      return RunlinkAddressResult::memory_fault;
    const std::uint64_t text_begin = candidate.module_base;
    const std::uint64_t text_end =
        text_begin + candidate.identity.text_size;
    if (candidate.module_base == 0U || candidate.identity.text_size == 0U ||
        guest_address < text_begin || guest_address >= text_end)
      continue;
    if (matched)
      return RunlinkAddressResult::duplicate_module;
    candidate.text_offset = guest_address - candidate.module_base;
    resolution = candidate;
    matched = true;
  }
  return matched ? RunlinkAddressResult::resolved
                 : RunlinkAddressResult::module_not_found;
}

RunlinkPublicationResult publish_runlink_module(
    hle::GuestMemory &memory, const std::uint32_t table_pointer_address,
    const std::size_t overlay_slot_count,
    const RunlinkModuleIdentity &identity,
    const std::uint32_t synthetic_base) noexcept {
  if (overlay_slot_count == 0U || synthetic_base == 0U ||
      overlay_slot_count >
          (std::numeric_limits<std::uint32_t>::max() /
           kRunlinkModuleRecordSize) - 1U)
    return RunlinkPublicationResult::invalid_argument;

  std::uint32_t table = 0U;
  if (!memory.read_u32(table_pointer_address, table))
    return RunlinkPublicationResult::memory_fault;
  if (table == 0U)
    return RunlinkPublicationResult::table_uninitialized;

  std::uint32_t matched_record = 0U;
  for (std::size_t slot = 1U; slot <= overlay_slot_count; ++slot) {
    const std::uint32_t record =
        table + static_cast<std::uint32_t>(slot) * kRunlinkModuleRecordSize;
    if (record < table)
      return RunlinkPublicationResult::memory_fault;
    std::uint32_t rom_start = 0U, text_size = 0U, data_size = 0U,
                  bss_size = 0U;
    if (!memory.read_u32(record + kRomStartOffset, rom_start) ||
        !memory.read_u32(record + kTextSizeOffset, text_size) ||
        !memory.read_u32(record + kDataSizeOffset, data_size) ||
        !memory.read_u32(record + kBssSizeOffset, bss_size))
      return RunlinkPublicationResult::memory_fault;
    if (rom_start != identity.rom_start || text_size != identity.text_size ||
        data_size != identity.data_size || bss_size != identity.bss_size)
      continue;
    if (matched_record != 0U)
      return RunlinkPublicationResult::duplicate_module;
    matched_record = record;
  }
  if (matched_record == 0U)
    return RunlinkPublicationResult::module_not_found;

  std::uint32_t current_base = 0U;
  if (!memory.read_u32(matched_record + kBaseOffset, current_base))
    return RunlinkPublicationResult::memory_fault;
  if (current_base != 0U)
    return RunlinkPublicationResult::already_published;
  if (!memory.write_u32(matched_record + kBaseOffset, synthetic_base))
    return RunlinkPublicationResult::memory_fault;
  return RunlinkPublicationResult::published;
}

RunlinkSuspensionResult resolve_runlink_suspension(
    const hle::GuestMemory& memory, const std::uint32_t pending_table,
    const std::size_t pending_count, const std::uint32_t module_slot) noexcept {
  if (pending_table == 0U || pending_count == 0U || pending_count > 16U ||
      module_slot == 0U || module_slot >= 0xFFBU)
    return RunlinkSuspensionResult::invalid;
  bool found = false;
  for (std::size_t index = 0U; index < pending_count; ++index) {
    const std::uint64_t address = std::uint64_t{pending_table} + index * 8U;
    if (address > UINT32_MAX - 7U) return RunlinkSuspensionResult::invalid;
    std::uint32_t base = 0U, slot = 0U;
    if (!memory.read_u32(static_cast<std::uint32_t>(address), base) ||
        !memory.read_u32(static_cast<std::uint32_t>(address) + 4U, slot))
      return RunlinkSuspensionResult::invalid;
    if (slot != module_slot) continue;
    std::uint32_t retained_word = 0U;
    if (found || base == 0U || !memory.read_u32(base, retained_word))
      return RunlinkSuspensionResult::invalid;
    found = true;
  }
  return found ? RunlinkSuspensionResult::suspended : RunlinkSuspensionResult::absent;
}

RunlinkRetentionResult resolve_runlink_retention(
    const hle::GuestMemory& memory, const std::uint32_t table_pointer_address,
    const std::size_t overlay_slot_count, const std::uint32_t pending_table,
    const std::size_t pending_count, const std::uint32_t module_slot) noexcept {
  if(module_slot==0U || module_slot>overlay_slot_count)
    return RunlinkRetentionResult::invalid;
  std::uint32_t table=0U,base=0U;
  if(!memory.read_u32(table_pointer_address,table) || table==0U)
    return RunlinkRetentionResult::invalid;
  const auto record=std::uint64_t{table}+std::uint64_t{module_slot}*kRunlinkModuleRecordSize;
  if(record>UINT32_MAX || !memory.read_u32(static_cast<std::uint32_t>(record),base))
    return RunlinkRetentionResult::invalid;
  if(base!=0U)return RunlinkRetentionResult::live;
  switch(resolve_runlink_suspension(memory,pending_table,pending_count,module_slot)) {
  case RunlinkSuspensionResult::suspended:return RunlinkRetentionResult::suspended;
  case RunlinkSuspensionResult::absent:return RunlinkRetentionResult::unloaded;
  default:return RunlinkRetentionResult::invalid;
  }
}

} // namespace jfg::boot
