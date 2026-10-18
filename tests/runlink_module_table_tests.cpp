#include "jfg/boot/runlink_module_table.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

constexpr std::uint32_t kPointer = 0x80000020U;
constexpr std::uint32_t kTable = 0x80000100U;
constexpr std::size_t kSlots = 3U;
constexpr jfg::boot::RunlinkModuleIdentity kTarget{
    0x01EFB128U, 0x2960U, 0x2D0U, 0x1B0U};

void check(const bool condition, const char *const message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::uint32_t record(const std::size_t slot) {
  return kTable + static_cast<std::uint32_t>(
                      slot * jfg::boot::kRunlinkModuleRecordSize);
}

void seed_identity(jfg::boot::hle::GuestMemory &memory,
                   const std::size_t slot,
                   const jfg::boot::RunlinkModuleIdentity &identity) {
  check(memory.write_u32(record(slot) + 4U, identity.rom_start), "seed rom");
  check(memory.write_u32(record(slot) + 8U, identity.text_size), "seed text");
  check(memory.write_u32(record(slot) + 12U, identity.data_size), "seed data");
  check(memory.write_u32(record(slot) + 16U, identity.bss_size), "seed bss");
}

void test_publish_and_preserve() {
  std::array<std::uint8_t, 2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  check(memory.write_u32(kPointer, kTable), "seed table pointer");
  seed_identity(memory, 2U, kTarget);
  check(jfg::boot::publish_runlink_module(memory, kPointer, kSlots, kTarget,
                                          0x00700000U) ==
            jfg::boot::RunlinkPublicationResult::published,
        "empty module published");
  std::uint32_t base = 0U;
  check(memory.read_u32(record(2U), base) && base == 0x00700000U,
        "synthetic base stored");
  check(memory.write_u32(record(2U), 0x80361CF0U), "seed real base");
  check(jfg::boot::publish_runlink_module(memory, kPointer, kSlots, kTarget,
                                          0x00700000U) ==
            jfg::boot::RunlinkPublicationResult::already_published,
        "real module preserved");
  check(memory.read_u32(record(2U), base) && base == 0x80361CF0U,
        "real base unchanged");
}

void test_uninitialized_and_not_found() {
  std::array<std::uint8_t, 2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  check(jfg::boot::publish_runlink_module(memory, kPointer, kSlots, kTarget,
                                          0x00700000U) ==
            jfg::boot::RunlinkPublicationResult::table_uninitialized,
        "zero table pointer is retryable");
  check(memory.write_u32(kPointer, kTable), "seed table pointer");
  check(jfg::boot::publish_runlink_module(memory, kPointer, kSlots, kTarget,
                                          0x00700000U) ==
            jfg::boot::RunlinkPublicationResult::module_not_found,
        "missing identity rejected");
}

void test_duplicate_and_fault() {
  std::array<std::uint8_t, 2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  check(memory.write_u32(kPointer, kTable), "seed table pointer");
  seed_identity(memory, 1U, kTarget);
  seed_identity(memory, 3U, kTarget);
  check(jfg::boot::publish_runlink_module(memory, kPointer, kSlots, kTarget,
                                          0x00700000U) ==
            jfg::boot::RunlinkPublicationResult::duplicate_module,
        "ambiguous identity rejected");

  check(memory.write_u32(kPointer, 0x800007E0U), "seed truncated table");
  check(jfg::boot::publish_runlink_module(memory, kPointer, kSlots, kTarget,
                                          0x00700000U) ==
            jfg::boot::RunlinkPublicationResult::memory_fault,
        "truncated table rejected");
}

void test_resolve_actual_text_address() {
  std::array<std::uint8_t, 2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  check(memory.write_u32(kPointer, kTable), "seed table pointer");
  seed_identity(memory, 2U, kTarget);
  check(memory.write_u32(record(2U), 0x803C0000U), "seed actual base");

  jfg::boot::RunlinkAddressResolution resolution{};
  check(jfg::boot::resolve_runlink_text_address(
            memory, kPointer, kSlots, 0x803C0120U, resolution) ==
            jfg::boot::RunlinkAddressResult::resolved,
        "actual text address resolves");
  check(resolution.module_base == 0x803C0000U &&
            resolution.text_offset == 0x120U &&
            resolution.identity.rom_start == kTarget.rom_start &&
            resolution.identity.text_size == kTarget.text_size,
        "resolution preserves identity, base, and offset");
  check(jfg::boot::resolve_runlink_text_address(
            memory, kPointer, kSlots,
            0x803C0000U + kTarget.text_size, resolution) ==
            jfg::boot::RunlinkAddressResult::module_not_found,
        "data address is not executable");
}

void test_resolve_rejects_ambiguous_and_invalid_tables() {
  std::array<std::uint8_t, 2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  jfg::boot::RunlinkAddressResolution resolution{};
  check(jfg::boot::resolve_runlink_text_address(
            memory, kPointer, kSlots, 0x803C0000U, resolution) ==
            jfg::boot::RunlinkAddressResult::table_uninitialized,
        "uninitialized address table is retryable");
  check(memory.write_u32(kPointer, kTable), "seed table pointer");
  seed_identity(memory, 1U, kTarget);
  seed_identity(memory, 2U, kTarget);
  check(memory.write_u32(record(1U), 0x803C0000U), "seed first base");
  check(memory.write_u32(record(2U), 0x803C0000U), "seed second base");
  check(jfg::boot::resolve_runlink_text_address(
            memory, kPointer, kSlots, 0x803C0010U, resolution) ==
            jfg::boot::RunlinkAddressResult::duplicate_module,
        "overlapping executable records fail closed");
}

} // namespace

void test_suspended_module_selection() {
  std::array<std::uint8_t, 2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  using Result = jfg::boot::RunlinkSuspensionResult;
  const auto resolve = [&](std::uint32_t slot) {
    return jfg::boot::resolve_runlink_suspension(memory, 0x80000400U, 16U, slot);
  };
  for (std::uint32_t i = 0U; i < 16U; ++i)
    check(memory.write_u32(0x80000404U + i * 8U, 0xFFBU), "seed unused pending entry");
  check(resolve(4U) == Result::absent, "cold module selects download");
  check(memory.write_u32(0x80000418U, 0x80000600U), "seed retained allocation");
  check(memory.write_u32(0x8000041CU, 4U), "seed suspended module");
  check(memory.write_u32(0x80000600U, 0x1234ABCDU), "seed mutable retained state");
  const auto before = bytes;
  check(resolve(4U) == Result::suspended, "pause module selects resume");
  check(bytes == before, "selection must preserve all retained bytes");
  check(resolve(5U) == Result::absent, "unrelated module remains cold");
  check(memory.write_u32(0x80000420U, 0x80000600U) &&
        memory.write_u32(0x80000424U, 4U), "seed duplicate");
  check(resolve(4U) == Result::invalid, "ambiguous suspension rejected");
  check(memory.write_u32(0x80000424U, 0xFFBU) && memory.write_u32(0x80000418U, 0U), "seed missing allocation");
  check(resolve(4U) == Result::invalid, "missing retained allocation rejected");
  check(resolve(0U) == Result::invalid && resolve(0xFFBU) == Result::invalid, "reserved slots rejected");
  check(jfg::boot::resolve_runlink_suspension(memory, 0xFFFFFFF8U, 16U, 4U) == Result::invalid,
        "out-of-range table rejected");
}

void test_full_unload_and_suspend_retention() {
  std::array<std::uint8_t,2048U> bytes{};
  jfg::boot::hle::GuestMemory memory(bytes);
  using Result=jfg::boot::RunlinkRetentionResult;
  check(memory.write_u32(kPointer,kTable),"seed lifetime table");
  for(std::uint32_t i=0U;i<16U;++i)
    check(memory.write_u32(0x80000404U+i*8U,0xFFBU),"seed empty retained slots");
  const auto resolve=[&] {
    return jfg::boot::resolve_runlink_retention(memory,kPointer,kSlots,0x80000400U,16U,2U);
  };
  check(memory.write_u32(record(2U),0x80000600U),"seed loaded module");
  check(resolve()==Result::live,"loaded module keeps mutable state");
  // A suspend frees executable memory while retaining data in the pending list.
  check(memory.write_u32(record(2U),0U) &&
        memory.write_u32(0x80000400U,0x80000600U) &&
        memory.write_u32(0x80000404U,2U) &&
        memory.write_u32(0x80000600U,0x80301234U),"suspend module with scratch pointer");
  const auto suspended_bytes=bytes;
  check(resolve()==Result::suspended,"zero executable base alone must not discard BSS");
  check(bytes==suspended_bytes,"retention check does not modify guest state");
  check(memory.write_u32(0x80000404U,0xFFBU),"remove retained module after full unload");
  check(resolve()==Result::unloaded,"full unload discards stale scratch pointers");
  check(memory.write_u32(0x80000404U,2U) && memory.write_u32(0x80000400U,0U),"seed invalid retained record");
  check(resolve()==Result::invalid,"malformed pending record cannot authorize discard");
  check(jfg::boot::resolve_runlink_retention(memory,kPointer,kSlots,0x80000400U,16U,0U)==Result::invalid,
        "reserved module cannot be retired");
  check(memory.write_u32(kPointer,0xFFFFFFF0U),"seed overflowing inventory");
  check(resolve()==Result::invalid,"overflowing inventory cannot authorize discard");
}

int main() {
  test_full_unload_and_suspend_retention();
  test_suspended_module_selection();
  test_publish_and_preserve();
  test_uninitialized_and_not_found();
  test_duplicate_and_fault();
  test_resolve_actual_text_address();
  test_resolve_rejects_ambiguous_and_invalid_tables();
  std::cout << "runlink module table tests passed\n";
  return 0;
}
