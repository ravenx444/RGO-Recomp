#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "jfg/runtime/snapshot_copy.hpp"
#include "funcs.h"
#if __has_include("jfg_original_timing.h")
#include "jfg_original_timing.h"
#else
#define JFG_ORIGINAL_TIMING_GENERATION 0
#include "jfg/boot/original_timing_fast.h"
#endif
#include "jfg/boot/hle.hpp"
#include "jfg/boot/mmio_instruction.hpp"
#include "jfg/boot/sync_print_sink.hpp"
#include "jfg/boot/sound_queue_recovery.hpp"
#include "jfg/boot/reset_handoff.hpp"
#include "jfg/boot/ipl_handoff.hpp"
#include "jfg/boot/gameplay_trace.hpp"
#include "jfg/mod/navigation_mod.hpp"
#include "jfg/mod/widescreen_hud.hpp"
#include "jfg/runtime/async_bounded_trace.hpp"
#include "jfg/runtime/async_snapshots.hpp"
#include "jfg/runtime/async_diagnostic_file.hpp"
#include "jfg/runtime/async_volume_file.hpp"
#include "jfg/audio/master_volume.hpp"
#include "jfg/audio/playback_buffer.hpp"
#include "jfg/audio/vi_playback_clock.hpp"
#include "jfg/runtime/window_input_mailbox.hpp"
#include "jfg/runtime/precise_wait.hpp"
#include <future>
#include <atomic>
#include "jfg/boot/runlink_module_table.hpp"
#include "jfg/boot/thread_scheduler.hpp"
#include "jfg/boot/guest_thread_transport.hpp"
#include "jfg/boot/tlb.hpp"
#include "jfg/boot/reference_cache.hpp"
#include "jfg/boot/cpu_status.hpp"
#include "jfg/boot/mi_interrupt_mask.hpp"
#include "jfg/boot/sp_status.hpp"
#include "jfg/boot/reference_sp_dma.hpp"
#include "jfg/boot/reference_pif_boot.hpp"
#include "jfg/boot/original_timing_pi_dma.hpp"
#include "jfg/boot/reference_compare.hpp"
#include "jfg/boot/reference_si_dma.hpp"
#include "jfg/boot/reference_event_commit.hpp"
#include "jfg/boot/reference_flash_bus.hpp"
#include "jfg/boot/original_timing_ai_dma.hpp"
#include "jfg/boot/point_probe.hpp"
#include "jfg/boot/device_event_probe.h"
#include "jfg/boot/eret_transfer_probe.hpp"
#include "jfg/boot/instruction_effect_trace.hpp"
#include "jfg/runtime/cpu_operation_bridge.h"
#include "jfg/boot/timers.hpp"
#include "jfg/boot/si_deadline.hpp"
#include "jfg/runtime/generated_overlay_runtime.hpp"
#include "jfg/runtime/input_stick.hpp"
#include "jfg/runtime/controller_mapping.hpp"
#include "jfg/runtime/controller_ports.hpp"
#include "jfg/runtime/pc_input.hpp"
#include "jfg/runtime/support_log.hpp"
#include "jfg/runtime/support_snapshot.hpp"
#include <iterator>
#include "jfg/runtime/rt64_overlay_address.hpp"
#include "jfg/renderer/rt64_f3ddkr_address.hpp"
#include "jfg/runtime/cic_nus_6105.hpp"
#include "jfg/testkernel/sha256.hpp"
#if defined(JFG_PHASE8_LIVE_RUNTIME)
#include "jfg/rgo/headless_autopilot.hpp"
#include "jfg/evidence/g2_private_task_adapter.hpp"
#include "jfg/renderer/rt64_shell.hpp"
#include "jfg/renderer/vi_presentation.hpp"
#include "jfg/renderer/frame_interpolation.hpp"
#include "jfg/renderer/frame_stats.hpp"
#include "jfg/renderer/widescreen.hpp"
#include "jfg/runtime/audio_task_bridge.hpp"
#include "jfg/runtime/input_replay.hpp"
#include "jfg/runtime/save_device_runtime.hpp"
#endif

#include <algorithm>
#include "jfg/runtime/controller_status_query.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <charconv>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <Xinput.h>
#include <fcntl.h>
#include <io.h>
#if defined(JFG_PHASE8_LIVE_RUNTIME)
#include <SDL.h>
#include "jfg/platform/sdl_gamepads.hpp"
#endif
extern "C" BOOLEAN WINAPI SystemFunction036(PVOID, ULONG);
#elif defined(JFG_HEADLESS_LIVE)
// RGO: the live runtime without a window or GPU renderer, for measuring the
// game's timing on Linux (docs/rgo/framerate.md, "Headless measurement").
#include "jfg/boot/headless_win32_compat.hpp"
#include <csignal>
#include <sys/mman.h>
#include <ucontext.h>
#if defined(JFG_PHASE8_LIVE_RUNTIME)
#include <SDL.h>
#include "jfg/platform/sdl_gamepads.hpp"
#endif
#endif
// The live runtime: Windows, or the headless Linux build.
#if defined(JFG_PHASE8_LIVE_RUNTIME) && (defined(_WIN32) || defined(JFG_HEADLESS_LIVE))
#define JFG_LIVE_RUNTIME 1
#endif

#if !defined(JFG_PHASE6_BUILD_IDENTITY)
#error "JFG_PHASE6_BUILD_IDENTITY must bind the native runner build"
#endif
#if !defined(JFG_PHASE6_SANITIZER_ID)
#error "JFG_PHASE6_SANITIZER_ID must bind the native runner instrumentation"
#endif

extern "C" {
JfgOriginalTimingFast* jfg_original_timing_state = nullptr;
std::uint32_t jfg_fast_budget = 0U;
std::uint32_t jfg_fast_fpu = 0U;
}

namespace jfg::boot::native {
extern "C" {
#include "jfg/boot/original_timing_cache.h"
}
namespace {
constexpr std::size_t kRomSize = 32U * 1024U * 1024U;
constexpr std::size_t kRdramSize = 4U * 1024U * 1024U;
constexpr std::size_t kKseg1AliasOffset = 0x20000000U;
constexpr std::size_t kCartCachedAliasOffset = 0x10000000U;
constexpr std::size_t kCartUncachedAliasOffset = 0x30000000U;
constexpr std::size_t kGuestAddressSpan = 0x8A000000U;
constexpr std::size_t kMmioPageSize = 0x1000U;
constexpr std::size_t kWindowsAllocationGranularity = 0x10000U;
constexpr std::array<std::size_t, 12U> kMmioHostOffsets = {
    0x24040000U, 0x24100000U, 0x24300000U, 0x24400000U,
    0x24500000U, 0x24600000U, 0x24700000U, 0x24800000U,
    0x24080000U, 0x3fc00000U, 0x28000000U, 0x28010000U,
};
constexpr std::uint32_t kKseg0 = 0x80000000U;
constexpr std::uint32_t kRunlinkModuleTablePointer = 0x800FEAA0U;
constexpr std::size_t kRunlinkOverlaySlotCount = 157U;
constexpr std::uint32_t kHintTextOverlaySection = 7U;
constexpr std::uint64_t kCountTicksPerViFrame = 781250U;
constexpr std::uint64_t kCountTicksPerDispatch = 64U;
// A guest may wait for asynchronous hardware by polling an empty libultra
// queue without blocking. Bound only that repeated poll pattern so ordinary
// generated execution keeps its established cooperative timing.
constexpr std::uint32_t kEmptyReceivePollTimeslice = 2048U;
constexpr std::array<std::uint8_t, 20U> kSupportedRomSha1 = {
    0x49U, 0x3cU, 0xedU, 0x90U, 0x08U, 0xdbU, 0xe9U, 0x32U, 0xd6U, 0xe9U,
    0x11U, 0x79U, 0xb6U, 0x8eU, 0x86U, 0x30U, 0xcfU, 0x23U, 0xa0U, 0x23U,
};
struct Mapping {
  std::uint32_t vram;
  const char *name;
};
constexpr Mapping kMappings[] = {
#include JFG_PHASE6_DISPATCH_TABLE_INC
};
constexpr std::size_t kMappingCount = sizeof(kMappings) / sizeof(kMappings[0]);
consteval std::uint32_t original_os_entry(std::string_view name) {
  for (const auto& mapping : kMappings)
    if (mapping.name == name) return mapping.vram;
  throw "Unknown original OS observer";
}

constexpr bool mappings_are_strictly_sorted() noexcept {
  for (std::size_t index = 1U; index < kMappingCount; ++index) {
    if (kMappings[index - 1U].vram >= kMappings[index].vram)
      return false;
  }
  return true;
}
static_assert(mappings_are_strictly_sorted(),
              "native HLE dispatch table must be sorted by guest address");

enum class NativeEventKind : std::uint32_t {
  kDispatch = 1U,
  kThreadCreated = 2U,
  kThreadStarted = 3U,
  kQueueCreated = 4U,
  kViConfigured = 5U,
  kViFrame = 6U,
  kViInterruptRaised = 7U,
  kViMessageDelivered = 8U,
  kViMessageConsumed = 9U,
  kMmioRead = 10U,
  kMmioWrite = 11U,
  kCacheMaintenance = 12U,
  kInterruptMask = 13U,
  kPiDma = 14U,
  kOverlayPublished = 15U,
  kController = 16U,
  kThreadPaused = 17U,
};

struct NativeEvent final {
  NativeEventKind kind = NativeEventKind::kDispatch;
  std::uint32_t a = 0U;
  std::uint32_t b = 0U;
  std::uint32_t c = 0U;
};

struct EventJournal final {
  static constexpr std::size_t kCapacity = 16384U;
  std::array<NativeEvent, kCapacity> entries{};
  std::array<std::uint64_t, 18U> observed_counts{};
  std::uint64_t observed_total = 0U;
  std::size_t count = 0U;
  bool overflow = false;

  void append(const NativeEventKind kind, const std::uint32_t a = 0U,
              const std::uint32_t b = 0U,
              const std::uint32_t c = 0U) noexcept {
    const std::size_t kind_index = static_cast<std::size_t>(kind);
    if (kind_index < observed_counts.size())
      ++observed_counts[kind_index];
    ++observed_total;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    // Live execution can run indefinitely, so its fixed-capacity journal keeps
    // structural events while the aggregate counters above retain exact totals
    // for high-frequency activity. Non-live Phase 6 evidence continues to
    // retain and hash every event exactly as before.
    switch (kind) {
    case NativeEventKind::kThreadCreated:
    case NativeEventKind::kThreadStarted:
    case NativeEventKind::kQueueCreated:
    case NativeEventKind::kViConfigured:
    case NativeEventKind::kOverlayPublished:
    case NativeEventKind::kThreadPaused:
      break;
    default:
      return;
    }
#endif
    if (count == entries.size()) {
      overflow = true;
      return;
    }
    entries[count++] = NativeEvent{kind, a, b, c};
  }
};

constexpr std::array<std::uint32_t, 12U> kMmioGuestBases = {
    0xA4040000U, 0xA4100000U, 0xA4300000U, 0xA4400000U,
    0xA4500000U, 0xA4600000U, 0xA4700000U, 0xA4800000U,
    0xA4080000U, 0xbfc00000U, 0xa8000000U, 0xa8010000U,
};
constexpr std::array<std::uint32_t, 12U> kMmioLastRegister = {
    0x1CU, 0x1CU, 0x0CU, 0x34U, 0x14U, 0x30U, 0x1CU, 0x1CU, 0U, 0x7fcU, 0U, 0U,
};

struct MmioTrace final {
  // Explicit diagnostic reference-device ownership. The cooperative control
  // continues using its existing register behavior unless this is enabled.
  bool guest_execution = false;
  std::uint64_t guest_count = 0U;
  std::uint64_t vi_next = 0U;
  // RGO fast clock: recent reads of the VI line register (a polling wait).
  std::uint64_t vi_line_poll_count = 0U;
  std::uint32_t vi_line_polls = 0U;
  std::uint64_t execution_io_revision = 0U;
  std::uint32_t vi_period = 500000U;
  std::uint32_t vi_v_sync = 0U;
  std::uint32_t vi_h_sync = 0U;
  std::uint32_t mi_pending = 0U;
  std::uint64_t vi_acknowledgements = 0U;
  jfg::boot::ReferenceSiDma si_dma;
  jfg::boot::OriginalTimingPiDma pi_dma;
  jfg::boot::ReferenceFlashBus flash_bus;
  jfg::boot::OriginalTimingAiDma ai_dma;
  std::uint64_t persisted_flash_mutations = 0;
  std::optional<std::uint64_t> sp_launch, sp_deadline, dp_deadline;
  std::span<std::uint8_t> device_rdram;
  std::span<const std::uint8_t> device_rom;
  const char* device_error = nullptr;
  std::uint32_t device_write_page = 0U, device_write_offset = 0U, device_write_value = 0U;
  std::uint32_t device_pi_offset = 0U, device_pi_value = 0U;
  std::uint32_t device_si_offset = 0U, device_si_value = 0U;
  std::uint64_t device_si_sequence = 0U, traced_si_sequence = 0U;
  std::optional<std::uint64_t> device_si_prior_deadline;
  std::uint64_t traced_pi_transfers = 0U;
  struct AiSubmission final {
    std::uint32_t address = 0U;
    std::uint32_t length = 0U;
    std::uint32_t dac_rate = 0U;
  };
  static constexpr std::size_t kAiSubmissionCapacity = 2048U;

  EventJournal *journal = nullptr;
  std::array<void *, kMmioHostOffsets.size()> pages{};
  void *pending_page = nullptr;
  std::size_t pending_page_index = SIZE_MAX;
  std::uintptr_t pending_address = 0U;
  bool pending_write = false;
  // The handler is process-wide. Only the thread that touched the guarded
  // page is single-stepping on our behalf; a trap-flag exception on any
  // other thread (debugger, GPU driver) must be left alone.
  std::uint32_t pending_thread = 0U;
  // RGO one-trap MMIO: perform plain 32-bit moves in the fault handler
  // instead of single-stepping them (mmio_emulate_move).
  bool emulate_moves = false;
  std::uint64_t emulated_accesses = 0U;
  std::uint64_t accesses = 0U;
  std::uint64_t unsupported_accesses = 0U;
  std::uint64_t ai_register_writes = 0U;
  std::uint64_t ai_buffer_submissions = 0U;
  std::uint32_t ai_dram_address = 0U;
  std::uint32_t ai_length = 0U;
  std::uint32_t ai_control = 0U;
  std::uint32_t ai_dac_rate = 0U;
  std::uint32_t ai_bit_rate = 0U;
  std::uint32_t ai_emulated_remaining_length = 0U;
  std::array<AiSubmission, kAiSubmissionCapacity> ai_submissions{};
  std::uint64_t ai_submission_write = 0U;
  std::uint64_t ai_submission_read = 0U;
  std::uint64_t ai_submission_overflows = 0U;
  bool guest_mi_mask = false;
  jfg::boot::MiInterruptMask mi_mask;
  bool guest_sp_status = false;
  jfg::boot::SpStatus sp_status;
  jfg::boot::ReferenceSpDma sp_dma;
  std::span<const std::uint8_t> sp_rdram;
  bool guest_pif_boot = false;
  jfg::boot::ReferencePifBoot pif_boot;

  void record(const std::size_t page_index, const std::uintptr_t address,
              const bool write) noexcept {
    ++accesses;
    const auto offset = static_cast<std::uint32_t>(
        address - reinterpret_cast<std::uintptr_t>(pages[page_index]));
    const bool supported = page_index == 9U ? guest_pif_boot && offset == 0x7fcU :
        (offset & 3U) == 0U && offset <= kMmioLastRegister[page_index];
    if (!supported)
      ++unsupported_accesses;
    if (journal != nullptr) {
      journal->append(write ? NativeEventKind::kMmioWrite
                            : NativeEventKind::kMmioRead,
                      kMmioGuestBases[page_index] + offset,
                      supported ? 1U : 0U, 4U);
    }
  }
};

#if defined(_WIN32) || defined(JFG_HEADLESS_LIVE)
MmioTrace *g_mmio_trace = nullptr;

// A guarded MMIO page was touched: present the register value for a read
// and arm the single step. Windows reports the access through a guard-page
// exception, the headless Linux build through SIGSEGV. Returns false for an
// unsupported access, which must not be resumed.
bool mmio_start_access(const std::size_t index, const std::uintptr_t address,
                       const bool write) noexcept {
  // RGO fast clock: device registers see the Count of this instruction.
  if (jfg_original_timing_state != nullptr)
    jfg_fast_run_sync(jfg_original_timing_state);
  const auto page = reinterpret_cast<std::uintptr_t>(g_mmio_trace->pages[index]);
  const auto offset = static_cast<std::uint32_t>(address - page);
  if (g_mmio_trace->guest_execution && !write && index >= 10U) {
    if (index != 10U || offset != 0U) g_mmio_trace->device_error = "unsupported-flash-read";
    const auto value = g_mmio_trace->flash_bus.status();
    std::memcpy(reinterpret_cast<void*>(address), &value, 4U);
  }
  if (g_mmio_trace->guest_execution && !write && index == 5U) {
    const auto value = g_mmio_trace->pi_dma.read(offset);
    if (!value) g_mmio_trace->device_error = "unsupported-pi-register";
    const auto result = value.value_or(0U);
    std::memcpy(reinterpret_cast<void*>(address), &result, 4U);
  }
  if (g_mmio_trace->guest_execution && !write && index == 4U) {
    const auto value = g_mmio_trace->ai_dma.read(offset, g_mmio_trace->guest_count);
    if (!value) g_mmio_trace->device_error = "unsupported-ai-register";
    const auto result = value.value_or(0U);
    std::memcpy(reinterpret_cast<void*>(address), &result, 4U);
  }
  if (g_mmio_trace->guest_execution && !write && index == 7U) {
    const auto value = g_mmio_trace->si_dma.read(offset);
    if (!value) g_mmio_trace->device_error = "unsupported-si-register";
    const auto result = value.value_or(0U);
    std::memcpy(reinterpret_cast<void*>(address), &result, 4U);
  }
  if (g_mmio_trace->guest_execution && !write && index == 2U && offset == 8U)
    std::memcpy(reinterpret_cast<void*>(address), &g_mmio_trace->mi_pending, 4U);
  if (g_mmio_trace->guest_execution && !write && index == 3U && offset == 0x10U) {
    // RGO fast clock: a loop polling the VI line (__osViInit waits for the
    // top of the screen) waits for the next VI; skip to it, as for an idle
    // loop. The line reads past the bottom until the VI event is taken.
    auto *const timing = jfg_original_timing_state;
    if (timing != nullptr && timing->fast_clock != 0U) {
      g_mmio_trace->vi_line_polls = g_mmio_trace->guest_count - g_mmio_trace->vi_line_poll_count <= 256U
          ? g_mmio_trace->vi_line_polls + 1U : 1U;
      g_mmio_trace->vi_line_poll_count = g_mmio_trace->guest_count;
      if (g_mmio_trace->vi_line_polls >= 16U && g_mmio_trace->vi_next > g_mmio_trace->guest_count) {
        jfg_fast_run_end(timing);
        timing->cpu_count = g_mmio_trace->vi_next;
        g_mmio_trace->guest_count = g_mmio_trace->vi_next;
        g_mmio_trace->vi_line_poll_count = g_mmio_trace->vi_next;
        g_mmio_trace->vi_line_polls = 0U;
      }
    }
    const auto origin = g_mmio_trace->vi_next - g_mmio_trace->vi_period;
    const auto line = g_mmio_trace->vi_period == 783520U ?
        static_cast<std::uint32_t>(((g_mmio_trace->guest_count - origin) * 263U) /
            g_mmio_trace->vi_period) * 2U :
        static_cast<std::uint32_t>((g_mmio_trace->guest_count - origin) / 1500U) & ~1U;
    std::memcpy(reinterpret_cast<void*>(address), &line, 4U);
  }
  if (index == 9U && (!g_mmio_trace->guest_pif_boot || offset != 0x7fcU)) {
    ++g_mmio_trace->unsupported_accesses;
    return false;
  }
  if (!write && index == 9U) {
    const auto control = g_mmio_trace->pif_boot.read();
    std::memcpy(reinterpret_cast<void*>(address), &control, 4U);
  }
  if (!write && g_mmio_trace->guest_sp_status && (index == 0U || index == 8U)) {
    const auto value = index == 8U ? (offset == 0U ?
        std::optional<std::uint32_t>{g_mmio_trace->sp_dma.pc()} : std::nullopt) :
        offset == 0x10U ? std::optional<std::uint32_t>{g_mmio_trace->sp_status.read()} :
        g_mmio_trace->sp_dma.read(offset);
    if (!value.has_value()) {
      ++g_mmio_trace->unsupported_accesses;
      return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), &*value, 4U);
  }
  if (!write && g_mmio_trace->guest_mi_mask && index == 2U && offset == 0x0cU) {
    const auto mask = g_mmio_trace->mi_mask.read();
    std::memcpy(reinterpret_cast<void*>(address), &mask, 4U);
  }
  if (!g_mmio_trace->guest_execution && !write && index == 4U && offset == 0x04U) {
    std::memcpy(reinterpret_cast<void *>(address),
                &g_mmio_trace->ai_emulated_remaining_length,
                sizeof(g_mmio_trace->ai_emulated_remaining_length));
  }
  g_mmio_trace->record(index, address, write);
  g_mmio_trace->pending_page = g_mmio_trace->pages[index];
  g_mmio_trace->pending_page_index = index;
  g_mmio_trace->pending_address = address;
  g_mmio_trace->pending_write = write;
  g_mmio_trace->pending_thread = GetCurrentThreadId();
  return true;
}

// The guest instruction that touched an MMIO page has retired (single step):
// apply a register write to the device models. Returns false for an
// unsupported access. The caller re-arms the page and clears the trap flag.
bool mmio_finish_access() noexcept {
  // RGO fast clock: a write may give a device a new deadline; end any skip
  // run. Reads change no device state.
  if (jfg_original_timing_state != nullptr && g_mmio_trace->pending_write)
    jfg_fast_run_end(jfg_original_timing_state);
  if (g_mmio_trace->pending_page_index == 9U && g_mmio_trace->pending_write) {
    std::uint32_t command = 0U;
    std::memcpy(&command, reinterpret_cast<const void*>(g_mmio_trace->pending_address), 4U);
    if (!g_mmio_trace->guest_pif_boot ||
        g_mmio_trace->pending_address - reinterpret_cast<std::uintptr_t>(g_mmio_trace->pending_page) != 0x7fcU ||
        !g_mmio_trace->pif_boot.write(command)) {
      ++g_mmio_trace->unsupported_accesses;
      return false;
    }
    const auto control = g_mmio_trace->pif_boot.read();
    std::memcpy(reinterpret_cast<void*>(g_mmio_trace->pending_address), &control, 4U);
    if (g_mmio_trace->guest_execution) {
      if (!g_mmio_trace->si_dma.boot_command(g_mmio_trace->guest_count)) {
        ++g_mmio_trace->unsupported_accesses;
        return false;
      }
      // Independently phased direct PIF-command-8 probe: SI assertion is
      // 2304 Count ticks after retirement, not immediate acknowledgement.
    }
  }
  if (g_mmio_trace->guest_sp_status && g_mmio_trace->pending_write &&
      (g_mmio_trace->pending_page_index == 0U ||
       g_mmio_trace->pending_page_index == 8U)) {
    std::uint32_t command = 0U;
    std::memcpy(&command, reinterpret_cast<const void*>(g_mmio_trace->pending_address), 4U);
    const auto offset = static_cast<std::uint32_t>(g_mmio_trace->pending_address -
        reinterpret_cast<std::uintptr_t>(g_mmio_trace->pending_page));
    const bool status_register = g_mmio_trace->pending_page_index == 0U && offset == 0x10U;
    const bool stopped = (g_mmio_trace->sp_status.read() & 0x1dU) == 1U;
    const bool valid = status_register ? g_mmio_trace->sp_status.command(command) :
        g_mmio_trace->pending_page_index == 8U ?
          (stopped && offset == 0U && g_mmio_trace->sp_dma.write_pc(command)) :
          (stopped && g_mmio_trace->sp_dma.write(offset, command, g_mmio_trace->sp_rdram));
    if (!valid) {
      ++g_mmio_trace->unsupported_accesses;
      return false;
    }
    if (status_register) {
      const auto status = g_mmio_trace->sp_status.read();
      if (g_mmio_trace->guest_execution) {
        g_mmio_trace->mi_pending = (g_mmio_trace->mi_pending & ~1U) |
            (g_mmio_trace->sp_status.interrupt() ? 1U : 0U);
        if (stopped && (status & 1U) == 0U) {
          if (g_mmio_trace->sp_launch || g_mmio_trace->sp_deadline)
            g_mmio_trace->device_error = "overlapping-sp-launch";
          else g_mmio_trace->sp_launch = g_mmio_trace->guest_count;
        }
      }
      std::memcpy(reinterpret_cast<void*>(g_mmio_trace->pending_address), &status, 4U);
    }
  }
  if (g_mmio_trace->guest_mi_mask && g_mmio_trace->pending_write &&
      g_mmio_trace->pending_page_index == 2U &&
      g_mmio_trace->pending_address ==
          reinterpret_cast<std::uintptr_t>(g_mmio_trace->pending_page) + 0x0cU) {
    std::uint32_t command = 0U;
    std::memcpy(&command, reinterpret_cast<const void*>(g_mmio_trace->pending_address), 4U);
    if (!g_mmio_trace->mi_mask.command(command)) {
      ++g_mmio_trace->unsupported_accesses;
      return false;
    }
    const auto mask = g_mmio_trace->mi_mask.read();
    std::memcpy(reinterpret_cast<void*>(g_mmio_trace->pending_address), &mask, 4U);
  }
  if (g_mmio_trace->guest_execution && g_mmio_trace->pending_write) {
    ++g_mmio_trace->execution_io_revision;
    const auto offset = static_cast<std::uint32_t>(g_mmio_trace->pending_address -
        reinterpret_cast<std::uintptr_t>(g_mmio_trace->pending_page));
    std::uint32_t value = 0U;
    std::memcpy(&value, reinterpret_cast<const void*>(g_mmio_trace->pending_address), 4U);
    g_mmio_trace->device_write_page = static_cast<std::uint32_t>(g_mmio_trace->pending_page_index);
    g_mmio_trace->device_write_offset = offset;
    g_mmio_trace->device_write_value = value;
    if (g_mmio_trace->pending_page_index == 2U && offset == 0U && (value & 0x800U))
      g_mmio_trace->mi_pending &= ~32U;
    if (g_mmio_trace->pending_page_index == 3U) {
      if (offset == 0x10U) {
        if (g_mmio_trace->mi_pending & 8U) ++g_mmio_trace->vi_acknowledgements;
        g_mmio_trace->mi_pending &= ~8U;
      }
      if (offset == 0x18U) g_mmio_trace->vi_v_sync = value;
      if (offset == 0x1cU) g_mmio_trace->vi_h_sync = value;
    }
    if (g_mmio_trace->pending_page_index == 7U) {
      g_mmio_trace->device_si_offset = offset;
      g_mmio_trace->device_si_value = value;
      g_mmio_trace->device_si_prior_deadline = g_mmio_trace->si_dma.deadline();
      ++g_mmio_trace->device_si_sequence;
      if (!g_mmio_trace->si_dma.write(offset, value, g_mmio_trace->guest_count,
              g_mmio_trace->device_rdram))
        g_mmio_trace->device_error = "unsupported-si-transaction";
      g_mmio_trace->mi_pending = (g_mmio_trace->mi_pending & ~2U) |
          (g_mmio_trace->si_dma.interrupt() ? 2U : 0U);
    }
    if (g_mmio_trace->pending_page_index == 4U) {
      if (!g_mmio_trace->ai_dma.write(offset, value, g_mmio_trace->guest_count,
              g_mmio_trace->device_rdram.size(), g_mmio_trace->vi_period))
        g_mmio_trace->device_error = "unsupported-ai-transaction";
      g_mmio_trace->mi_pending = (g_mmio_trace->mi_pending & ~4U) |
          (g_mmio_trace->ai_dma.interrupt() ? 4U : 0U);
    }
    if (g_mmio_trace->pending_page_index == 5U) {
      g_mmio_trace->device_pi_offset = offset;
      g_mmio_trace->device_pi_value = value;
      if (!g_mmio_trace->pi_dma.write(offset, value, g_mmio_trace->guest_count,
              g_mmio_trace->device_rdram, g_mmio_trace->device_rom,
              [](void* device, bool to_ram, std::uint32_t cart, std::uint32_t dram,
                 std::uint32_t length, std::span<std::uint8_t> ram) {
                return static_cast<jfg::boot::ReferenceFlashBus*>(device)->transfer(to_ram, cart, dram, length, ram);
              }, &g_mmio_trace->flash_bus))
        g_mmio_trace->device_error = "unsupported-pi-transaction";
      g_mmio_trace->mi_pending = (g_mmio_trace->mi_pending & ~16U) |
          (g_mmio_trace->pi_dma.interrupt() ? 16U : 0U);
    }
    if (g_mmio_trace->pending_page_index >= 10U) {
      const bool valid = offset == 0U && (g_mmio_trace->pending_page_index == 10U ?
          g_mmio_trace->flash_bus.write_status(value) :
          g_mmio_trace->pending_page_index == 11U &&
            g_mmio_trace->flash_bus.command(value, g_mmio_trace->device_rdram));
      if (!valid) g_mmio_trace->device_error = "unsupported-flash-command";
    }
  }
  if (g_mmio_trace->pending_write &&
      g_mmio_trace->pending_page_index == 4U) {
    const auto page = reinterpret_cast<std::uintptr_t>(
        g_mmio_trace->pending_page);
    const auto offset = static_cast<std::uint32_t>(
        g_mmio_trace->pending_address - page);
    std::uint32_t value = 0U;
    std::memcpy(&value,
                reinterpret_cast<const void *>(
                    g_mmio_trace->pending_address),
                sizeof(value));
    ++g_mmio_trace->ai_register_writes;
    switch (offset) {
    case 0x00U:
      g_mmio_trace->ai_dram_address = value;
      break;
    case 0x04U:
      g_mmio_trace->ai_length = value;
      ++g_mmio_trace->ai_buffer_submissions;
      if (value != 0U) {
        if (g_mmio_trace->ai_submission_write -
                g_mmio_trace->ai_submission_read >=
            MmioTrace::kAiSubmissionCapacity) {
          ++g_mmio_trace->ai_submission_overflows;
        } else {
          const std::size_t slot = static_cast<std::size_t>(
              g_mmio_trace->ai_submission_write %
              MmioTrace::kAiSubmissionCapacity);
          g_mmio_trace->ai_submissions[slot] = {
              g_mmio_trace->ai_dram_address, value,
              g_mmio_trace->ai_dac_rate};
          ++g_mmio_trace->ai_submission_write;
        }
      }
      break;
    case 0x08U:
      g_mmio_trace->ai_control = value;
      break;
    case 0x10U:
      g_mmio_trace->ai_dac_rate = value;
      break;
    case 0x14U:
      g_mmio_trace->ai_bit_rate = value;
      break;
    default:
      break;
    }
  }
  return true;
}

void mmio_clear_pending() noexcept {
  g_mmio_trace->pending_page = nullptr;
  g_mmio_trace->pending_page_index = SIZE_MAX;
  g_mmio_trace->pending_address = 0U;
  g_mmio_trace->pending_write = false;
  g_mmio_trace->pending_thread = 0U;
}

// RGO one-trap MMIO (docs/rgo/framerate.md). Called after
// mmio_start_access on the opened page: when the faulting host instruction
// is a plain 32-bit move, do its load or store on the page here, so the
// caller can finish the access at once and skip the instruction instead of
// single-stepping it. The register page and the order (start, move, finish)
// are the same as with single-stepping. Returns the instruction length, or 0
// when the instruction is not one it decodes (the caller single-steps).
template <typename RegisterSlot>
std::uint32_t mmio_emulate_move(const std::uintptr_t address, const bool write,
                                const std::uint8_t *code,
                                RegisterSlot &&register_slot) noexcept {
  if (!g_mmio_trace->emulate_moves || (address & 3U) != 0U || code == nullptr)
    return 0U;
  const auto instruction = jfg::boot::decode_mmio_instruction(code);
  if (instruction.length == 0U || (write ? !instruction.store() : !instruction.load()))
    return 0U;
  std::uint64_t *const slot = register_slot(instruction.reg);
  if (slot == nullptr)
    return 0U;
  if (write) {
    const std::uint32_t word = jfg::boot::mmio_store_word(instruction, *slot);
    std::memcpy(reinterpret_cast<void *>(address), &word, sizeof(word));
  } else {
    std::uint32_t word = 0U;
    std::memcpy(&word, reinterpret_cast<const void *>(address), sizeof(word));
    *slot = jfg::boot::mmio_load_result(instruction, word);
  }
  ++g_mmio_trace->emulated_accesses;
  return instruction.length;
}
#endif

#if defined(_WIN32)
LONG CALLBACK mmio_exception_handler(EXCEPTION_POINTERS *exception) {
  if (g_mmio_trace == nullptr || exception == nullptr ||
      exception->ExceptionRecord == nullptr ||
      exception->ContextRecord == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = exception->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_SINGLE_STEP &&
      g_mmio_trace->pending_page != nullptr &&
      g_mmio_trace->pending_thread == GetCurrentThreadId()) {
    if (!mmio_finish_access())
      return EXCEPTION_CONTINUE_SEARCH;
    DWORD previous = 0U;
    (void)VirtualProtect(g_mmio_trace->pending_page, kMmioPageSize,
                         PAGE_READWRITE | PAGE_GUARD, &previous);
    mmio_clear_pending();
    exception->ContextRecord->EFlags &= ~DWORD{0x100U};
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code != STATUS_GUARD_PAGE_VIOLATION ||
      exception->ExceptionRecord->NumberParameters < 2U)
    return EXCEPTION_CONTINUE_SEARCH;
  const auto address = static_cast<std::uintptr_t>(
      exception->ExceptionRecord->ExceptionInformation[1]);
  for (std::size_t index = 0U; index < g_mmio_trace->pages.size(); ++index) {
    const auto page = reinterpret_cast<std::uintptr_t>(
        g_mmio_trace->pages[index]);
    if (address < page || address >= page + kMmioPageSize)
      continue;
    const bool write =
        exception->ExceptionRecord->ExceptionInformation[0] == 1U;
    if (!mmio_start_access(index, address, write))
      return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *const registers = exception->ContextRecord;
    const std::uint32_t length = mmio_emulate_move(
        address, write, reinterpret_cast<const std::uint8_t *>(registers->Rip),
        [registers](const std::uint8_t number) -> std::uint64_t * {
          DWORD64 *slot = nullptr;
          switch (number) {
          case 0U: slot = &registers->Rax; break;
          case 1U: slot = &registers->Rcx; break;
          case 2U: slot = &registers->Rdx; break;
          case 3U: slot = &registers->Rbx; break;
          case 4U: slot = &registers->Rsp; break;
          case 5U: slot = &registers->Rbp; break;
          case 6U: slot = &registers->Rsi; break;
          case 7U: slot = &registers->Rdi; break;
          case 8U: slot = &registers->R8; break;
          case 9U: slot = &registers->R9; break;
          case 10U: slot = &registers->R10; break;
          case 11U: slot = &registers->R11; break;
          case 12U: slot = &registers->R12; break;
          case 13U: slot = &registers->R13; break;
          case 14U: slot = &registers->R14; break;
          case 15U: slot = &registers->R15; break;
          default: break;
          }
          return reinterpret_cast<std::uint64_t *>(slot);
        });
    if (length != 0U) {
      // The move is done: finish now and re-arm the guard, no single step.
      if (!mmio_finish_access())
        return EXCEPTION_CONTINUE_SEARCH;
      DWORD previous = 0U;
      (void)VirtualProtect(g_mmio_trace->pending_page, kMmioPageSize,
                           PAGE_READWRITE | PAGE_GUARD, &previous);
      mmio_clear_pending();
      registers->Rip += length;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    registers->EFlags |= DWORD{0x100U};
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#elif defined(JFG_HEADLESS_LIVE)
// Linux: MMIO pages are PROT_NONE while armed. SIGSEGV opens the page and
// sets the x86-64 trap flag; SIGTRAP after the instruction retires applies a
// write and closes the page again, as the Windows guard-page handler does.
struct sigaction g_mmio_previous_segv{};
struct sigaction g_mmio_previous_trap{};

// Set once the game runs: prints the guest calls that led to a crash.
void (*g_crash_report)(int) noexcept = nullptr;

[[noreturn]] void mmio_fail(const int signal_number) noexcept {
  static bool reported = false;
  if (g_crash_report != nullptr && !reported) {
    reported = true;
    g_crash_report(signal_number);
  }
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
  ::_exit(128 + signal_number);
}

void mmio_segv_handler(int signal_number, siginfo_t *info, void *context) {
  auto *const user = static_cast<ucontext_t *>(context);
  const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
  if (g_mmio_trace != nullptr && g_mmio_trace->pending_page == nullptr) {
    for (std::size_t index = 0U; index < g_mmio_trace->pages.size(); ++index) {
      const auto page = reinterpret_cast<std::uintptr_t>(g_mmio_trace->pages[index]);
      if (page == 0U || address < page || address >= page + kMmioPageSize)
        continue;
      // Page-fault error code bit 1: the access was a write.
      const bool write = (user->uc_mcontext.gregs[REG_ERR] & 2) != 0;
      if (::mprotect(g_mmio_trace->pages[index], kMmioPageSize,
                     PROT_READ | PROT_WRITE) != 0 ||
          !mmio_start_access(index, address, write))
        mmio_fail(signal_number);
      auto &registers = user->uc_mcontext.gregs;
      const std::uint32_t length = mmio_emulate_move(
          address, write, reinterpret_cast<const std::uint8_t *>(registers[REG_RIP]),
          [&registers](const std::uint8_t number) -> std::uint64_t * {
            static constexpr std::array<int, 16U> kSlots = {
                REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
            static_assert(sizeof(greg_t) == sizeof(std::uint64_t));
            return number < kSlots.size()
                       ? reinterpret_cast<std::uint64_t *>(&registers[kSlots[number]])
                       : nullptr;
          });
      if (length != 0U) {
        // The move is done: finish now and close the page, no single step.
        if (!mmio_finish_access()) mmio_fail(signal_number);
        (void)::mprotect(g_mmio_trace->pending_page, kMmioPageSize, PROT_NONE);
        mmio_clear_pending();
        registers[REG_RIP] += length;
        return;
      }
      registers[REG_EFL] |= 0x100;
      return;
    }
  }
  mmio_fail(signal_number);
}

void mmio_trap_handler(int signal_number, siginfo_t *, void *context) {
  auto *const user = static_cast<ucontext_t *>(context);
  if (g_mmio_trace == nullptr || g_mmio_trace->pending_page == nullptr ||
      g_mmio_trace->pending_thread != GetCurrentThreadId())
    mmio_fail(signal_number);
  if (!mmio_finish_access()) mmio_fail(signal_number);
  (void)::mprotect(g_mmio_trace->pending_page, kMmioPageSize, PROT_NONE);
  mmio_clear_pending();
  user->uc_mcontext.gregs[REG_EFL] &= ~0x100;
}

bool install_mmio_signal_handlers() noexcept {
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = mmio_segv_handler;
  if (::sigaction(SIGSEGV, &action, &g_mmio_previous_segv) != 0) return false;
  action.sa_sigaction = mmio_trap_handler;
  return ::sigaction(SIGTRAP, &action, &g_mmio_previous_trap) == 0;
}

void remove_mmio_signal_handlers() noexcept {
  (void)::sigaction(SIGSEGV, &g_mmio_previous_segv, nullptr);
  (void)::sigaction(SIGTRAP, &g_mmio_previous_trap, nullptr);
}
#endif

#if defined(JFG_LIVE_RUNTIME)
class HostAudioDevice final {
public:
  HostAudioDevice() = default;
  ~HostAudioDevice() { close(); }
  HostAudioDevice(const HostAudioDevice &) = delete;
  HostAudioDevice &operator=(const HostAudioDevice &) = delete;

  void set_gameplay_trace(GameplayTrace *trace) noexcept { gameplay_trace_ = trace; }
  void frontend_pause(bool paused) noexcept {
    if (device_ != 0U && device_playing_) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
  }
  void set_vi_counter(const std::uint64_t *vi) noexcept { capture_vi_ = vi; }
  void renderer_ready() noexcept { renderer_ready_ = true; update_playback_state(); }

  bool configure_capture(const std::string &path) {
    capture_pcm_.open(path, std::ios::binary | std::ios::trunc);
    capture_events_.open(path + ".events.csv",
                         std::ios::binary | std::ios::trunc);
    if (!capture_pcm_ || !capture_events_)
      return false;
    capture_started_ = std::chrono::steady_clock::now();
    capture_events_ << "wall_us,event,queued_bytes,total_queued_bytes,"
                       "consumed_bytes,vi,steady_us\n";
    return true;
  }

  bool queue(const std::uint8_t *const rdram, std::uint32_t address,
             const std::uint32_t length, const std::uint32_t dac_rate) {
    constexpr std::uint32_t kNtscVideoClock = 48681812U;
    address &= 0x00FFFFFFU;
    if (rdram == nullptr || length == 0U || (length & 3U) != 0U ||
        address > kRdramSize || length > kRdramSize - address ||
        dac_rate == UINT32_MAX)
      return false;
    const std::uint32_t frequency =
        kNtscVideoClock / (dac_rate + 1U);
    if (frequency < 8000U || frequency > 96000U ||
        (!initialized_ && !initialize(frequency)) || frequency_ != frequency)
      return false;

    refresh_volume(frequency);
    update_metrics();
    converted_.resize(length);
    for (std::uint32_t offset = 0U; offset < length; offset += 2U) {
      const std::uint16_t sample = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(
               rdram[(address + offset) ^ 3U]) << 8U) |
          rdram[(address + offset + 1U) ^ 3U]);
      converted_[offset] = static_cast<std::uint8_t>(sample);
      converted_[offset + 1U] = static_cast<std::uint8_t>(sample >> 8U);
    }
    if (capture_pcm_.is_open()) {
      capture_pcm_.write(
          reinterpret_cast<const char *>(converted_.data()),
          static_cast<std::streamsize>(converted_.size()));
      if (!capture_pcm_)
        return false;
    }
    master_volume_.apply(converted_);
    if (device_ != 0U &&
        SDL_QueueAudio(device_, converted_.data(), length) != 0)
      return false;
    ++submitted_buffers_;
    queued_bytes_ += length;
    emulated_dma_lengths_.push_back(length);
    update_metrics();
    capture_event("queue");
    update_playback_state();
    return true;
  }

  // Shader startup may accumulate more than the configured priming cushion.
  // Establish the wall clock only after that excess has played, so startup
  // work cannot permanently choose a larger audio/video phase offset.
  void settle_startup_queue() {
    if (!device_playing_ || (device_ == 0U && !wall_sink_)) return;
    update_metrics();
    capture_event("anchor-drain-begin");
    while (device_playing_ && current_queued_bytes_ > prebuffer_bytes_) {
      SDL_Delay(1U);
      update_metrics();
      update_playback_state();
    }
    capture_event("anchor-drain-end");
  }

  void service(const bool advance_emulated_clock, const bool throttle) {
    if (!initialized_)
      return;
    if (advance_emulated_clock)
      advance_emulated_dma();
    update_metrics();
    update_playback_state();
    if (throttle && device_playing_ && current_queued_bytes_ > pacing_bytes_) {
      const auto throttle_start = std::chrono::steady_clock::now();
      while (throttle && device_playing_ &&
             current_queued_bytes_ > pacing_bytes_) {
        SDL_Delay(1U);
        update_metrics();
        update_playback_state();
      }
      throttled_us_ += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - throttle_start).count());
    }
    if (!advance_emulated_clock) {
      if (capture_pcm_.is_open())
        capture_pcm_.flush();
      if (capture_events_.is_open())
        capture_events_.flush();
    }
  }

  void finish_capture() noexcept { capture_pcm_.finish(); capture_events_.finish(); }

  void close() noexcept {
    if (device_ != 0U) {
      update_metrics();
      SDL_PauseAudioDevice(device_, 1);
      SDL_CloseAudioDevice(device_);
      device_ = 0U;
    }
    initialized_ = false;
  }

  [[nodiscard]] bool initialized() const noexcept { return initialized_; }
  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] std::uint32_t frequency() const noexcept { return frequency_; }
  [[nodiscard]] std::uint64_t submitted_buffers() const noexcept {
    return submitted_buffers_;
  }
  [[nodiscard]] std::uint64_t queued_bytes() const noexcept {
    return queued_bytes_;
  }
  [[nodiscard]] std::uint64_t consumed_bytes() const noexcept {
    return consumed_bytes_;
  }
  [[nodiscard]] std::uint64_t underruns() const noexcept {
    return underruns_;
  }
  [[nodiscard]] std::uint64_t overruns() const noexcept { return overruns_; }
  // Time spent waiting for a too-full queue to play down (RGO busy time).
  [[nodiscard]] std::uint64_t throttled_microseconds() const noexcept { return throttled_us_; }
  // The cushion the queue is refilled to after an underrun, in milliseconds
  // (100 at first, more after underruns; the Detailed counter shows it).
  [[nodiscard]] std::uint32_t cushion_milliseconds() const noexcept {
    return frequency_ == 0U ? 0U : static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(resume_bytes_) * 1000U / (frequency_ * 4U));
  }
  // Audio waiting to play, in milliseconds (headless counter log).
  [[nodiscard]] std::uint32_t queued_milliseconds() const noexcept {
    return frequency_ == 0U ? 0U : static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(current_queued_bytes_) * 1000U / (frequency_ * 4U));
  }
  // RGO: how many nanoseconds earlier (positive) or later (negative) this
  // VI's deadline should be to hold the queue at the cushion over a long
  // session (jfg::audio::QueueLevelTrim). Zero while the device is not playing.
  [[nodiscard]] std::int64_t level_trim(const std::int64_t vi_nanoseconds) noexcept {
    if (!device_playing_ || frequency_ == 0U || (device_ == 0U && !wall_sink_)) {
      level_trim_.reset();
      return 0;
    }
    return level_trim_.update(current_queued_bytes_, resume_bytes_, frequency_ * 4U,
                              vi_nanoseconds);
  }
  // The queue level the trim works from (smoothed over about half a second).
  [[nodiscard]] std::uint32_t smoothed_queue_milliseconds() const noexcept {
    return frequency_ == 0U || !device_playing_ ? 0U : static_cast<std::uint32_t>(
        level_trim_.smoothed_bytes() * 1000.0 / (static_cast<double>(frequency_) * 4.0));
  }
  [[nodiscard]] std::uint64_t consumed_milliseconds() const noexcept {
    return frequency_ == 0U
               ? 0U
               : consumed_bytes_ * 1000U /
                     (static_cast<std::uint64_t>(frequency_) * 4U);
  }
  [[nodiscard]] std::uint32_t current_buffer_remaining() const noexcept {
    return emulated_dma_lengths_.empty() ? 0U
                                         : emulated_dma_lengths_.front();
  }

private:
  void refresh_volume(const std::uint32_t frequency) {
    if (!volume_path_checked_) {
      volume_path_checked_ = true;
      wchar_t *path = nullptr;
      std::size_t length = 0;
      if (_wdupenv_s(&path, &length, L"JFG_MASTER_VOLUME_CONFIG") == 0 && path) {
        volume_path_ = std::filesystem::path(path);
        std::free(path);
        volume_file_.start(volume_path_);
      }
    }
    if (volume_path_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < volume_next_read_) return;
    volume_next_read_ = now + std::chrono::milliseconds(100);
    // Runs on the existing producer thread, never an SDL audio callback.
    // Missing/partial replacements keep the last accepted preference.
    const auto text = volume_file_.latest();
    if (!text) return;
    if (const auto settings = jfg::audio::parse_volume(*text)) {
      master_volume_.configure(*settings, frequency, !volume_received_);
      volume_received_ = true;
    }
  }

  bool initialize(const std::uint32_t frequency) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
      return false;
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(frequency);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2U;
    desired.samples = 256U;
    SDL_AudioSpec obtained{};
#if defined(JFG_HEADLESS_LIVE)
    // RGO headless measurement: JFG_RGO_WALL_AUDIO=1 plays into an exact
    // wall-clock sink instead of an SDL device (SDL's dummy driver consumes
    // a few percent too fast or too slow, which shows as false underruns).
    if (const char *wall = std::getenv("JFG_RGO_WALL_AUDIO"); wall != nullptr && wall[0] == '1') {
      wall_sink_ = true;
      obtained = desired;
      // JFG_RGO_WALL_AUDIO_PPM: the sink's clock runs this many parts per
      // million fast (negative: slow), like a sound card's crystal.
      if (const char *ppm = std::getenv("JFG_RGO_WALL_AUDIO_PPM"); ppm != nullptr)
        wall_rate_ = 1.0 + std::strtod(ppm, nullptr) / 1e6;
    } else
#endif
    device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (wall_sink_) {
      // The wall-clock sink stands in for the device.
    } else if (device_ == 0U || obtained.freq != desired.freq ||
        obtained.format != desired.format ||
        obtained.channels != desired.channels) {
      if (device_ != 0U)
        SDL_CloseAudioDevice(device_);
      device_ = 0U;
      // A missing or mismatched host device is a correctness failure by
      // default so a manual run cannot silently lose audio. Headless soak
      // hosts opt in to a null sink whose consumption follows the emulated
      // AI DMA clock, keeping the simulation deterministic without SDL.
      if (!null_sink_requested())
        return false;
      null_sink_ = true;
    }
    frequency_ = frequency;
    // Give the first renderer/shader work a generous cushion, then keep normal
    // host latency bounded independently of the emulated AI DMA clock.
    prebuffer_bytes_ = frequency_ * 4U / 10U;
    resume_bytes_ = frequency_ * 4U / 10U;
    pacing_bytes_ = frequency_ * 4U / 5U;
    overrun_bytes_ = frequency_ * 4U * 2U;
    initialized_ = true;
    return true;
  }

  [[nodiscard]] static bool null_sink_requested() noexcept {
    char *value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, "JFG_PHASE9_NULL_AUDIO") != 0 ||
        value == nullptr)
      return false;
    const bool requested = value[0] == '1' && value[1] == '\0';
    std::free(value);
    return requested;
  }

  void update_metrics() noexcept {
    if (wall_sink_) {
      // Consume at exactly the output rate while playing.
      const auto now = std::chrono::steady_clock::now();
      if (device_playing_ && wall_mark_ != std::chrono::steady_clock::time_point{})
        wall_consumed_ += std::chrono::duration<double>(now - wall_mark_).count() *
                          static_cast<double>(frequency_) * 4.0 * wall_rate_;
      wall_mark_ = now;
      if (wall_consumed_ > static_cast<double>(queued_bytes_))
        wall_consumed_ = static_cast<double>(queued_bytes_);
      consumed_bytes_ = static_cast<std::uint64_t>(wall_consumed_) & ~std::uint64_t{3};
      current_queued_bytes_ = static_cast<std::uint32_t>(
          (std::min)(queued_bytes_ - consumed_bytes_, std::uint64_t{UINT32_MAX}));
    } else if (device_ == 0U) {
      if (!null_sink_)
        return;
      std::uint64_t remaining = 0U;
      for (const std::uint32_t length : emulated_dma_lengths_)
        remaining += length;
      current_queued_bytes_ = static_cast<std::uint32_t>(
          (std::min)(remaining, std::uint64_t{UINT32_MAX}));
      consumed_bytes_ = queued_bytes_ >= current_queued_bytes_
                            ? queued_bytes_ - current_queued_bytes_
                            : 0U;
      return;
    }
    if (!wall_sink_) {
      current_queued_bytes_ = SDL_GetQueuedAudioSize(device_);
      consumed_bytes_ = queued_bytes_ >= current_queued_bytes_
                            ? queued_bytes_ - current_queued_bytes_
                            : 0U;
    }
    if (device_playing_ && current_queued_bytes_ == 0U) {
      if (!empty_episode_) {
        ++underruns_;
        empty_episode_ = true;
        capture_event("underrun");
      }
    } else if (current_queued_bytes_ != 0U) {
      empty_episode_ = false;
    }
    if (current_queued_bytes_ > overrun_bytes_) {
      if (!overrun_episode_) {
        ++overruns_;
        overrun_episode_ = true;
      }
    } else {
      overrun_episode_ = false;
    }
  }

  void update_playback_state() noexcept {
    if (device_ == 0U && !wall_sink_) {
      if (null_sink_ && !started_ &&
          current_queued_bytes_ >= prebuffer_bytes_)
        started_ = true;
      return;
    }
    if (!started_ && !renderer_ready_) return;
    switch (jfg::audio::playback_buffer_action(started_, device_playing_,
                current_queued_bytes_, prebuffer_bytes_, resume_bytes_)) {
    case jfg::audio::PlaybackBufferAction::start:
      if (device_ != 0U) SDL_PauseAudioDevice(device_, 0);
      started_ = true;
      device_playing_ = true;
      empty_episode_ = false;
      capture_event("start");
      break;
    case jfg::audio::PlaybackBufferAction::pause_empty:
      if (device_ != 0U) SDL_PauseAudioDevice(device_, 1);
      device_playing_ = false;
      // RGO: the queue ran dry; rebuffer a larger cushion so a stall of the
      // same length does not cut out again (playback_buffer.hpp).
      resume_bytes_ = jfg::audio::grown_cushion(resume_bytes_, frequency_ * 4U / 20U,
                                                frequency_ * 4U / 5U);
      prebuffer_bytes_ = resume_bytes_;
      capture_event("pause-empty");
      break;
    case jfg::audio::PlaybackBufferAction::resume:
      if (device_ != 0U) SDL_PauseAudioDevice(device_, 0);
      device_playing_ = true;
      empty_episode_ = false;
      capture_event("resume");
      break;
    case jfg::audio::PlaybackBufferAction::none:
      break;
    }
  }

  void capture_event(const char *event) noexcept {
    if (gameplay_trace_ != nullptr && *gameplay_trace_) {
      std::array<char, 48> name{};
      (void)std::snprintf(name.data(), name.size(), "audio-%s", event);
      gameplay_trace_->event(name.data(), {current_queued_bytes_, queued_bytes_,
          consumed_bytes_, frequency_, device_playing_ ? 1U : 0U});
    }
    if (!capture_events_.is_open())
      return;
    const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - capture_started_).count();
    capture_events_ << wall_us << ',' << event << ','
                    << current_queued_bytes_ << ',' << queued_bytes_ << ','
                    << consumed_bytes_ << ',' << (capture_vi_ ? *capture_vi_ : 0U) << ','
                    << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
  }

  void advance_emulated_dma() noexcept {
    constexpr std::uint32_t kViRetracesPerSecond = 60U;
    emulated_byte_accumulator_ +=
        static_cast<std::uint64_t>(frequency_) * 4U;
    std::uint64_t bytes =
        emulated_byte_accumulator_ / kViRetracesPerSecond;
    emulated_byte_accumulator_ %= kViRetracesPerSecond;
    while (bytes != 0U && !emulated_dma_lengths_.empty()) {
      std::uint32_t &remaining = emulated_dma_lengths_.front();
      if (bytes < remaining) {
        remaining -= static_cast<std::uint32_t>(bytes);
        bytes = 0U;
      } else {
        bytes -= remaining;
        emulated_dma_lengths_.pop_front();
      }
    }
  }

  GameplayTrace *gameplay_trace_ = nullptr;
  const std::uint64_t *capture_vi_ = nullptr;
  SDL_AudioDeviceID device_ = 0U;
  jfg::audio::MasterVolume master_volume_;
  std::filesystem::path volume_path_;
  jfg::AsyncVolumeFile volume_file_;
  std::chrono::steady_clock::time_point volume_next_read_{};
  bool volume_path_checked_ = false, volume_received_ = false;
  std::vector<std::uint8_t> converted_;
  jfg::AsyncDiagnosticFile capture_pcm_;
  jfg::AsyncDiagnosticFile capture_events_;
  std::chrono::steady_clock::time_point capture_started_{};
  std::deque<std::uint32_t> emulated_dma_lengths_;
  std::uint64_t emulated_byte_accumulator_ = 0U;
  std::uint32_t frequency_ = 0U;
  std::uint64_t throttled_us_ = 0U;
  jfg::audio::QueueLevelTrim level_trim_;
  // RGO headless wall-clock sink (JFG_RGO_WALL_AUDIO).
  bool wall_sink_ = false;
  double wall_consumed_ = 0.0;
  double wall_rate_ = 1.0;
  std::chrono::steady_clock::time_point wall_mark_{};
  std::uint32_t prebuffer_bytes_ = 0U;
  std::uint32_t resume_bytes_ = 0U;
  std::uint32_t pacing_bytes_ = 0U;
  std::uint32_t overrun_bytes_ = 0U;
  std::uint32_t current_queued_bytes_ = 0U;
  bool renderer_ready_ = false;
  std::uint64_t submitted_buffers_ = 0U;
  std::uint64_t queued_bytes_ = 0U;
  std::uint64_t consumed_bytes_ = 0U;
  std::uint64_t underruns_ = 0U;
  std::uint64_t overruns_ = 0U;
  bool initialized_ = false;
  bool started_ = false;
  bool device_playing_ = false;
  bool empty_episode_ = false;
  bool overrun_episode_ = false;
  bool null_sink_ = false;
};
#endif

class GuestBacking final {
public:
  GuestBacking() = default;
  ~GuestBacking() {
#if defined(_WIN32)
    if (exception_handler_ != nullptr)
      (void)RemoveVectoredExceptionHandler(exception_handler_);
    if (g_mmio_trace == &mmio_trace_)
      g_mmio_trace = nullptr;
    if (alias_ != nullptr)
      UnmapViewOfFile(alias_);
    if (cart_cached_ != nullptr)
      UnmapViewOfFile(cart_cached_);
    if (cart_uncached_ != nullptr)
      UnmapViewOfFile(cart_uncached_);
    if (base_ != nullptr)
      UnmapViewOfFile(base_);
    for (void *page : mmio_)
      if (page != nullptr)
        (void)VirtualFree(page, 0U, MEM_RELEASE);
    for (void *region : overlays_)
      if (region != nullptr)
        (void)VirtualFree(region, 0U, MEM_RELEASE);
    release_gaps();
    if (mapping_ != nullptr)
      CloseHandle(mapping_);
    if (rom_mapping_ != nullptr)
      CloseHandle(rom_mapping_);
#elif defined(JFG_HEADLESS_LIVE)
    if (handlers_installed_)
      remove_mmio_signal_handlers();
    if (g_mmio_trace == &mmio_trace_)
      g_mmio_trace = nullptr;
    if (span_ != nullptr)
      (void)::munmap(span_, kGuestAddressSpan);
#endif
  }
  GuestBacking(const GuestBacking &) = delete;
  GuestBacking &operator=(const GuestBacking &) = delete;

  bool create() {
#if defined(_WIN32)
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                  PAGE_READWRITE, 0U,
                                  static_cast<DWORD>(kRdramSize), nullptr);
    rom_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0U,
                                      static_cast<DWORD>(kRomSize), nullptr);
    if (mapping_ == nullptr || rom_mapping_ == nullptr)
      return false;
    for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
      void *candidate = VirtualAlloc(
          nullptr, kGuestAddressSpan, MEM_RESERVE, PAGE_NOACCESS);
      if (candidate == nullptr)
        return false;
      (void)VirtualFree(candidate, 0U, MEM_RELEASE);
      base_ = static_cast<std::uint8_t *>(MapViewOfFileEx(
          mapping_, FILE_MAP_ALL_ACCESS, 0U, 0U, kRdramSize, candidate));
      if (base_ == nullptr)
        continue;
      void *alias_address = reinterpret_cast<void *>(
          reinterpret_cast<std::uintptr_t>(base_) + kKseg1AliasOffset);
      alias_ = static_cast<std::uint8_t *>(MapViewOfFileEx(
          mapping_, FILE_MAP_ALL_ACCESS, 0U, 0U, kRdramSize, alias_address));
      void *const cart_cached_address = reinterpret_cast<void *>(
          reinterpret_cast<std::uintptr_t>(base_) + kCartCachedAliasOffset);
      cart_cached_ = static_cast<std::uint8_t *>(MapViewOfFileEx(
          rom_mapping_, FILE_MAP_ALL_ACCESS, 0U, 0U, kRomSize,
          cart_cached_address));
      void *const cart_uncached_address = reinterpret_cast<void *>(
          reinterpret_cast<std::uintptr_t>(base_) + kCartUncachedAliasOffset);
      cart_uncached_ = static_cast<std::uint8_t *>(MapViewOfFileEx(
          rom_mapping_, FILE_MAP_READ, 0U, 0U, kRomSize,
          cart_uncached_address));
      bool mmio_ok = alias_ != nullptr && cart_cached_ == cart_cached_address &&
                     cart_uncached_ == cart_uncached_address;
      for (std::size_t index = 0U; mmio_ok && index < mmio_.size(); ++index) {
        void *const page_address = reinterpret_cast<void *>(
            reinterpret_cast<std::uintptr_t>(base_) + kMmioHostOffsets[index]);
        mmio_[index] = VirtualAlloc(page_address, kMmioPageSize,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        mmio_ok = mmio_[index] == page_address;
      }
      if (mmio_ok)
        {
          overlays_.assign(jfg_generated_section_count(), nullptr);
          bool overlays_ok = true;
          for (std::uint32_t section = 0U;
               overlays_ok && section < overlays_.size(); ++section) {
            JfgGeneratedSectionMetadata metadata{};
            if (jfg_generated_section_metadata(section, &metadata) == 0) {
              overlays_ok = false;
              break;
            }
            if (metadata.is_overlay != 1U)
              continue;
            const std::uint64_t extent = std::uint64_t{metadata.text_size} +
                                         metadata.data_size +
                                         metadata.bss_size;
            const std::uint64_t allocation =
                (extent + kWindowsAllocationGranularity - 1U) &
                ~(std::uint64_t{kWindowsAllocationGranularity} - 1U);
            const std::uint64_t host_offset =
                std::uint64_t{0x80000000U} + metadata.linked_vram;
            if (extent == 0U || allocation > SIZE_MAX ||
                host_offset > kGuestAddressSpan ||
                allocation > kGuestAddressSpan - host_offset) {
              overlays_ok = false;
              break;
            }
            void *const overlay_address = reinterpret_cast<void *>(
                reinterpret_cast<std::uintptr_t>(base_) + host_offset);
            overlays_[section] = VirtualAlloc(
                overlay_address, static_cast<std::size_t>(allocation),
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            overlays_ok = overlays_[section] == overlay_address;
          }
          // The span was released so the views could be placed; anything
          // the host allocated into a gap since then would be reachable by
          // a wild guest pointer instead of faulting. Re-reserve every
          // remaining free gap as no-access memory, and treat a gap that is
          // no longer free as a failed attempt.
          if (overlays_ok && reserve_gaps())
            return true;
        }
      release_gaps();
      for (void *&region : overlays_) {
        if (region != nullptr)
          (void)VirtualFree(region, 0U, MEM_RELEASE);
        region = nullptr;
      }
      overlays_.clear();
      for (void *&page : mmio_) {
        if (page != nullptr)
          (void)VirtualFree(page, 0U, MEM_RELEASE);
        page = nullptr;
      }
      if (alias_ != nullptr)
        UnmapViewOfFile(alias_);
      alias_ = nullptr;
      if (cart_cached_ != nullptr)
        UnmapViewOfFile(cart_cached_);
      cart_cached_ = nullptr;
      if (cart_uncached_ != nullptr)
        UnmapViewOfFile(cart_uncached_);
      cart_uncached_ = nullptr;
      UnmapViewOfFile(base_);
      base_ = nullptr;
    }
    return false;
#elif defined(JFG_HEADLESS_LIVE)
    return create_headless();
#else
    bytes_.resize(kRdramSize);
    return true;
#endif
  }
  std::span<std::uint8_t> bytes() noexcept {
#if defined(_WIN32) || defined(JFG_HEADLESS_LIVE)
    return {base_, base_ == nullptr ? 0U : kRdramSize};
#else
    return bytes_;
#endif
  }

#if defined(_WIN32)
  [[nodiscard]] std::size_t reserved_gap_count() const noexcept {
    return gaps_.size();
  }

private:
  bool reserve_gaps() {
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(base_);
    const std::uintptr_t end = begin + kGuestAddressSpan;
    std::uintptr_t cursor = begin;
    while (cursor < end) {
      MEMORY_BASIC_INFORMATION info{};
      if (VirtualQuery(reinterpret_cast<void *>(cursor), &info,
                       sizeof(info)) == 0U)
        return false;
      const std::uintptr_t region_begin =
          reinterpret_cast<std::uintptr_t>(info.BaseAddress);
      const std::uintptr_t region_end = region_begin + info.RegionSize;
      if (info.State == MEM_FREE) {
        // Reservations are placed at allocation granularity. A free tail
        // below the next 64 KiB boundary cannot be claimed by any other
        // allocation either, so it is safe to leave unreserved.
        const std::uintptr_t gap_begin =
            ((std::max)(region_begin, cursor) +
             kWindowsAllocationGranularity - 1U) &
            ~(std::uintptr_t{kWindowsAllocationGranularity} - 1U);
        const std::uintptr_t gap_end = (std::min)(region_end, end);
        if (gap_begin < gap_end) {
          void *const reserved = VirtualAlloc(
              reinterpret_cast<void *>(gap_begin), gap_end - gap_begin,
              MEM_RESERVE, PAGE_NOACCESS);
          if (reserved != reinterpret_cast<void *>(gap_begin)) {
            if (reserved != nullptr)
              (void)VirtualFree(reserved, 0U, MEM_RELEASE);
            return false;
          }
          gaps_.push_back(reserved);
        }
      }
      if (region_end <= cursor)
        return false;
      cursor = region_end;
    }
    return true;
  }

  void release_gaps() noexcept {
    for (void *gap : gaps_)
      if (gap != nullptr)
        (void)VirtualFree(gap, 0U, MEM_RELEASE);
    gaps_.clear();
  }

public:
#endif

  bool arm_mmio_monitor(EventJournal &journal) noexcept {
#if defined(_WIN32) || defined(JFG_HEADLESS_LIVE)
    // JFG_RGO_MMIO_SINGLE_STEP=1 single-steps every access, as before.
    char single_step[4]{};
    mmio_trace_.emulate_moves =
        GetEnvironmentVariableA("JFG_RGO_MMIO_SINGLE_STEP", single_step,
                                static_cast<DWORD>(sizeof(single_step))) == 0U ||
        single_step[0] != '1';
#endif
#if defined(_WIN32)
    mmio_trace_.journal = &journal;
    mmio_trace_.pages = mmio_;
    g_mmio_trace = &mmio_trace_;
    exception_handler_ = AddVectoredExceptionHandler(1U,
                                                      mmio_exception_handler);
    if (exception_handler_ == nullptr) {
      g_mmio_trace = nullptr;
      return false;
    }
    for (void *page : mmio_) {
      DWORD previous = 0U;
      if (page == nullptr ||
          VirtualProtect(page, kMmioPageSize, PAGE_READWRITE | PAGE_GUARD,
                         &previous) == 0)
        return false;
    }
    return true;
#elif defined(JFG_HEADLESS_LIVE)
    mmio_trace_.journal = &journal;
    mmio_trace_.pages = mmio_;
    g_mmio_trace = &mmio_trace_;
    if (!install_mmio_signal_handlers()) {
      g_mmio_trace = nullptr;
      return false;
    }
    handlers_installed_ = true;
    for (void *page : mmio_)
      if (page == nullptr || ::mprotect(page, kMmioPageSize, PROT_NONE) != 0)
        return false;
    return true;
#else
    (void)journal;
    return false;
#endif
  }

  [[nodiscard]] const MmioTrace &mmio_trace() const noexcept {
    return mmio_trace_;
  }
  [[nodiscard]] MmioTrace &mmio_trace() noexcept { return mmio_trace_; }

  bool load_overlay_images(
      const std::span<const std::uint8_t> rom) noexcept {
#if defined(_WIN32)
    if (rom.size() != kRomSize || cart_cached_ == nullptr ||
        cart_uncached_ == nullptr ||
        overlays_.size() != jfg_generated_section_count())
      return false;
    for (std::size_t index = 0U; index < rom.size(); ++index)
      cart_cached_[index ^ 3U] = rom[index];
    DWORD previous = 0U;
    if (VirtualProtect(cart_cached_, kRomSize, PAGE_READONLY, &previous) == 0 ||
        VirtualProtect(cart_uncached_, kRomSize, PAGE_READONLY, &previous) == 0)
      return false;
    for (std::uint32_t section = 0U; section < overlays_.size(); ++section) {
      JfgGeneratedSectionMetadata metadata{};
      if (jfg_generated_section_metadata(section, &metadata) == 0)
        return false;
      if (metadata.is_overlay != 1U)
        continue;
      const std::uint64_t initialized =
          std::uint64_t{metadata.text_size} + metadata.data_size;
      if (overlays_[section] == nullptr || metadata.rom_start > rom.size() ||
          initialized > rom.size() - metadata.rom_start)
        return false;
      auto *destination = static_cast<std::uint8_t *>(overlays_[section]);
      for (std::size_t index = 0U;
           index < static_cast<std::size_t>(initialized); ++index) {
        destination[index ^ 3U] = rom[metadata.rom_start + index];
      }
    }
    return true;
#elif defined(JFG_HEADLESS_LIVE)
    if (rom.size() != kRomSize || cart_cached_ == nullptr ||
        cart_uncached_ == nullptr ||
        overlays_.size() != jfg_generated_section_count())
      return false;
    for (std::size_t index = 0U; index < rom.size(); ++index)
      cart_cached_[index ^ 3U] = rom[index];
    if (::mprotect(cart_cached_, kRomSize, PROT_READ) != 0)
      return false;
    for (std::uint32_t section = 0U; section < overlays_.size(); ++section) {
      JfgGeneratedSectionMetadata metadata{};
      if (jfg_generated_section_metadata(section, &metadata) == 0)
        return false;
      if (metadata.is_overlay != 1U)
        continue;
      const std::uint64_t initialized =
          std::uint64_t{metadata.text_size} + metadata.data_size;
      if (overlays_[section] == nullptr || metadata.rom_start > rom.size() ||
          initialized > rom.size() - metadata.rom_start)
        return false;
      auto *destination = static_cast<std::uint8_t *>(overlays_[section]);
      for (std::size_t index = 0U;
           index < static_cast<std::size_t>(initialized); ++index) {
        destination[index ^ 3U] = rom[metadata.rom_start + index];
      }
    }
    return true;
#else
    (void)rom;
    return false;
#endif
  }

private:
#if defined(_WIN32)
  HANDLE mapping_ = nullptr;
  HANDLE rom_mapping_ = nullptr;
  std::uint8_t *base_ = nullptr;
  std::uint8_t *alias_ = nullptr;
  std::uint8_t *cart_cached_ = nullptr;
  std::uint8_t *cart_uncached_ = nullptr;
  std::array<void *, kMmioHostOffsets.size()> mmio_{};
  std::vector<void *> overlays_;
  std::vector<void *> gaps_;
  PVOID exception_handler_ = nullptr;
#elif defined(JFG_HEADLESS_LIVE)
  // One no-access reservation of the guest span. RDRAM and the cartridge
  // are shared-memory files mapped at their KSEG0/KSEG1 offsets inside it,
  // so every alias sees the same bytes, as with the Windows file views.
  bool create_headless() {
    const int rdram_file = ::memfd_create("jfg-rdram", 0U);
    const int rom_file = ::memfd_create("jfg-cartridge", 0U);
    bool ok = rdram_file >= 0 && rom_file >= 0 &&
        ::ftruncate(rdram_file, static_cast<off_t>(kRdramSize)) == 0 &&
        ::ftruncate(rom_file, static_cast<off_t>(kRomSize)) == 0;
    void *const span = ok ? ::mmap(nullptr, kGuestAddressSpan, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) : MAP_FAILED;
    ok = ok && span != MAP_FAILED;
    if (ok) {
      span_ = static_cast<std::uint8_t *>(span);
      const auto view = [&](std::size_t offset, std::size_t size, int file,
                            int protection) -> std::uint8_t * {
        void *const address = span_ + offset;
        void *const mapped = file >= 0
            ? ::mmap(address, size, protection, MAP_SHARED | MAP_FIXED, file, 0)
            : ::mmap(address, size, protection,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return mapped == address ? static_cast<std::uint8_t *>(mapped) : nullptr;
      };
      base_ = view(0U, kRdramSize, rdram_file, PROT_READ | PROT_WRITE);
      alias_ = view(kKseg1AliasOffset, kRdramSize, rdram_file,
                    PROT_READ | PROT_WRITE);
      cart_cached_ = view(kCartCachedAliasOffset, kRomSize, rom_file,
                          PROT_READ | PROT_WRITE);
      cart_uncached_ = view(kCartUncachedAliasOffset, kRomSize, rom_file,
                            PROT_READ);
      ok = base_ != nullptr && alias_ != nullptr && cart_cached_ != nullptr &&
           cart_uncached_ != nullptr;
      for (std::size_t index = 0U; ok && index < mmio_.size(); ++index) {
        mmio_[index] = view(kMmioHostOffsets[index], kMmioPageSize, -1,
                            PROT_READ | PROT_WRITE);
        ok = mmio_[index] != nullptr;
      }
      if (ok)
        overlays_.assign(jfg_generated_section_count(), nullptr);
      for (std::uint32_t section = 0U; ok && section < overlays_.size();
           ++section) {
        JfgGeneratedSectionMetadata metadata{};
        if (jfg_generated_section_metadata(section, &metadata) == 0) {
          ok = false;
          break;
        }
        if (metadata.is_overlay != 1U)
          continue;
        const std::uint64_t extent = std::uint64_t{metadata.text_size} +
                                     metadata.data_size + metadata.bss_size;
        const std::uint64_t allocation =
            (extent + kWindowsAllocationGranularity - 1U) &
            ~(std::uint64_t{kWindowsAllocationGranularity} - 1U);
        const std::uint64_t host_offset =
            std::uint64_t{0x80000000U} + metadata.linked_vram;
        if (extent == 0U || host_offset > kGuestAddressSpan ||
            allocation > kGuestAddressSpan - host_offset) {
          ok = false;
          break;
        }
        overlays_[section] = view(static_cast<std::size_t>(host_offset),
            static_cast<std::size_t>(allocation), -1, PROT_READ | PROT_WRITE);
        ok = overlays_[section] != nullptr;
      }
    }
    // The views keep the files alive.
    if (rdram_file >= 0) (void)::close(rdram_file);
    if (rom_file >= 0) (void)::close(rom_file);
    return ok;
  }

  std::uint8_t *span_ = nullptr;
  std::uint8_t *base_ = nullptr;
  std::uint8_t *alias_ = nullptr;
  std::uint8_t *cart_cached_ = nullptr;
  std::uint8_t *cart_uncached_ = nullptr;
  std::array<void *, kMmioHostOffsets.size()> mmio_{};
  std::vector<void *> overlays_;
  bool handlers_installed_ = false;
#else
  std::vector<std::uint8_t> bytes_;
#endif
  MmioTrace mmio_trace_{};
};

class Table final : public jfg::GeneratedOverlayTable {
public:
  std::size_t section_count() const override {
    return jfg_generated_section_count();
  }
  bool initialize_sections(std::span<std::int32_t> addresses) override {
    return jfg_generated_initialize_sections(addresses.data(),
                                             addresses.size()) != 0;
  }
  bool section_extents(std::uint32_t section,
                       jfg::GeneratedSectionExtents &output) const override {
    JfgGeneratedSectionMetadata metadata{};
    if (jfg_generated_section_metadata(section, &metadata) == 0 ||
        metadata.is_overlay > 1U)
      return false;
    output = {metadata.text_size, metadata.data_size, metadata.bss_size,
              metadata.is_overlay == 1U};
    return true;
  }
  bool update_section_lifecycle(jfg::GeneratedSectionLifecycle operation,
                                std::uint32_t section,
                                std::int32_t base) override {
    return jfg_generated_section_lifecycle(
               operation == jfg::GeneratedSectionLifecycle::load ? 1U : 2U,
               section, base) == 0;
  }
  std::size_t relocation_count(std::uint32_t section) const override {
    return jfg_generated_relocation_count(section);
  }
  bool relocation_sites(std::uint32_t section,
                        std::span<const std::uint32_t> &output) const override {
    const std::uint32_t *sites = nullptr;
    std::size_t count = 0U;
    if (jfg_generated_relocation_sites(section, &sites, &count) == 0 ||
        count != relocation_count(section) || (count != 0U && sites == nullptr))
      return false;
    output = {sites, count};
    return true;
  }
  bool
  relocation_descriptors(std::uint32_t section,
                         std::span<const jfg::GeneratedR32RelocationDescriptor>
                             &output) const override {
    const JfgGeneratedR32Descriptor *raw = nullptr;
    std::size_t count = 0U;
    if (jfg_generated_relocation_descriptors(section, &raw, &count) == 0 ||
        count != relocation_count(section) || (count != 0U && raw == nullptr))
      return false;
    descriptors_.clear();
    descriptors_.reserve(count);
    for (std::size_t index = 0U; index < count; ++index)
      descriptors_.push_back({raw[index].site_offset, raw[index].target_section,
                              raw[index].target_offset});
    output = descriptors_;
    return true;
  }
  jfg::GeneratedRelocationResult
  apply_relocations_checked(std::span<std::uint8_t> rdram,
                            std::uint32_t section,
                            std::size_t expected) override {
    const bool applied = jfg_generated_apply_relocations_checked(
                             rdram.data(), rdram.size(), section) != 0;
    return {applied, applied ? expected : 0U};
  }
  jfg::GeneratedOverlayFunction
  lookup_function(std::int32_t address) const override {
    return jfg_generated_lookup_function(address);
  }

private:
  mutable std::vector<jfg::GeneratedR32RelocationDescriptor> descriptors_;
};

#if defined(JFG_PHASE8_LIVE_RUNTIME)
struct PendingLiveGraphicsTask final {
  jfg::Rt64GraphicsTask task{};
  std::array<std::uint32_t, 6U> descriptor{};
  std::vector<std::byte> rdram;
  bool skipped = false;
  // RGO host speed: this snapshot's id, the id of the snapshot its buffer
  // held before (0: a new buffer), and the 4 KB chunks that differ from it.
  std::uint64_t snapshot_id = 0U;
  std::uint64_t base_id = 0U;
  jfg::SnapshotChanges changes;
};

// A graphics snapshot buffer waiting for reuse, with the id of the snapshot
// it holds.
struct PooledGraphicsSnapshot final {
  std::vector<std::byte> rdram;
  std::uint64_t id = 0U;
};

struct LiveGraphicsOverlayShadow final {
  std::uint32_t section = 0U;
  std::uint32_t linked_base = 0U;
  std::uint32_t extent = 0U;
  std::uint32_t physical_base = 0U;
};

struct LiveViFieldRegisters final {
  std::uint32_t origin = 640U;
  std::uint32_t y_scale = 0x00000400U;
  std::uint32_t vertical_start = 0x002501FFU;
  std::uint32_t vertical_burst = 0x000E0204U;
  std::uint32_t intr = 2U;
};
#endif

struct State : JfgOriginalTimingFast {
  State() : JfgOriginalTimingFast{} { execution_fast_revision = UINT64_MAX; }
  bool ledgered = false;
  bool os_initialized = false;
  jfg::GeneratedOverlayRuntime *runtime = nullptr;
  const std::uint8_t *rom = nullptr;
  std::size_t rom_size = 0U;
  std::unique_ptr<ThreadScheduler> scheduler;
  std::unordered_map<std::uint32_t, int> threads;
  std::vector<std::pair<std::uint32_t, int>> thread_order;
  std::vector<std::uint32_t> thread_entries;
  std::unordered_set<std::uint32_t> queues;
  std::unordered_set<std::uint32_t> active_overlay_sections;
  // Linked section metadata is immutable for this generated root. Dynamic
  // allocation/publication checks still run for every dispatched call.
  std::unordered_map<std::uint32_t,
      std::pair<std::uint32_t, JfgGeneratedSectionMetadata>> generated_section_cache;
  std::unordered_set<std::uint32_t> guest_overlay_loads_in_progress;
  jfg::ControllerMapping controller_mapping{};
  jfg::ControllerPorts controller_ports;
  jfg::ControllerInputGate controller_input_gate;
  std::filesystem::path controller_config_path;
  std::chrono::steady_clock::time_point controller_config_check{};
  std::uint32_t executing_generated_section = UINT32_MAX;
  bool executing_generated_overlay = false;
  const char *last_overlay_publication_failure = "none";
  std::unordered_set<std::uint32_t> generated_relocation_targets;
  std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>
      events;
  std::uint32_t pi_command_queue = 0U;
  bool vi_manager_thread_created = false;
  std::uint32_t vi_mode = 0U;
  std::uint32_t vi_queue = 0U;
  std::uint32_t vi_message = 0U;
  std::uint32_t vi_retrace_interval = 0U;
  std::uint32_t vi_current_framebuffer = 0U;
  std::uint32_t vi_next_framebuffer = 0U;
  std::uint32_t vi_special_features = 0U;
  bool vi_blacked = true;
  std::uint64_t vi_retraces = 0U;
  std::uint64_t vi_frames = 0U;
  std::uint64_t vi_interrupts = 0U;
  std::uint64_t vi_messages_delivered = 0U;
  std::uint32_t retrace_target = 3U;
  std::uint32_t poll_target = 0U;
  std::uint32_t vi_internal_queue = 0U;
  std::uint32_t vi_internal_message = 0U;
  EventJournal journal;
  MmioTrace *mmio_trace = nullptr;
  bool scheduler_running = false;
  std::uint32_t root_priority = 0U;
  std::uint32_t interrupt_mask = 0U;
  Timers timers;
  std::uint64_t os_time_base = 0U;
  std::uint64_t os_time_count_origin = 0U;
  std::uint32_t last_pi_rom = 0U;
  std::uint32_t last_pi_dram = 0U;
  std::uint32_t last_pi_size = 0U;
  bool controller_initialized = false;
  bool controller_guest_init_probe = false;
  std::uint32_t pending_si_completions = 0U;
  SiDeadline si_deadline;
  // Private generator-hook observer. This does not feed Count or deadlines.
  bool si_count_probe = false;
  bool renderer_writeback_probe = false;
  bool guest_leaf_probe = false;
  bool guest_initializing = false;
  bool guest_os_probe = false;
  bool timing_fixture = false;
  bool original_os_host_pacing = false;
  std::uint64_t original_window_pump_vi = UINT64_MAX;
  bool original_os_host_anchor_ready = false;
  jfg::audio::ViPlaybackClock original_os_host_clock;

  std::unique_ptr<GuestThreadTransport<recomp_context>> guest_transport;
  std::uint32_t guest_epc = 0U, guest_cause = 0U;
  jfg::boot::ReferenceCompare guest_timer;
  std::uint32_t guest_active_rsp_task = 0U, guest_active_rsp_type = 0U;
  std::uint64_t guest_exceptions = 0U;
  jfg::AsyncDiagnosticFile guest_clock_trace;
  std::unique_ptr<FILE, decltype(&std::fclose)> device_event_stream{nullptr, &std::fclose};
  jfg_device_event_probe device_event_probe{};
  std::ofstream eret_transfer_stream;
  EretTransferProbe eret_transfer_probe;
  std::ofstream instruction_effect_stream;
  InstructionEffectTrace instruction_effect_trace;
  std::uint32_t instruction_effect_update = 0U, instruction_effect_section = 0U;
  std::uint32_t device_event_previous_pc = 0U, device_event_previous_word = 0U;
  std::uint32_t device_event_previous_thread = 0U;
  bool device_event_successor = false;
  jfg::boot::TlbRegisters32 tlb_registers;
  std::vector<std::uint32_t> saved_rcp_masks;
  std::array<std::uint8_t, jfg::kPifRamBytes> pif_ram{};
  bool cic_response_pending = false;
  std::uint64_t generated_calls = 0U;
  std::uint64_t mapped_calls = 0U;
  std::uint64_t dispatch_calls = 0U;
  std::uint64_t sync_print_sink_calls = 0U;
  std::uint64_t sound_queue_recoveries = 0U;
  std::uint64_t phase9_player_control_calls = 0U;
  std::uint64_t phase9_weapon_update_calls = 0U;
  std::uint64_t phase9_weapon_fire_held_updates = 0U;
  std::uint64_t phase9_weapon_fire_pressed_updates = 0U;
  std::uint64_t phase9_weapon_dummy_fire_calls = 0U;
  std::uint64_t phase9_weapon_fire_calls = 0U;
  std::uint64_t phase9_weapon_shot_calls = 0U;
  std::uint64_t phase9_weapon_hit_calls = 0U;
  std::uint64_t phase9_enemy_kill_calls = 0U;
  std::uint64_t phase9_collision_calls = 0U;
  std::uint64_t phase9_particle_update_calls = 0U;
  std::uint64_t phase9_cutscene_active_calls = 0U;
  std::uint64_t phase9_cutscene_camera_calls = 0U;
  std::uint64_t phase9_level_change_calls = 0U;
  std::uint64_t phase9_death_restart_calls = 0U;
  std::uint64_t health_overlay_death_reload_generation = 0U;
  std::uint64_t phase9_player_hit_check_calls = 0U;
  std::uint32_t phase9_player_actor = 0U;
  jfg::mod::NavigationMod navigation_mod;
  std::filesystem::path navigation_output;
  jfg::AsyncBoundedTrace movement_trace, jump_trace;
  jfg::AsyncSnapshots snapshot_writer;
  std::shared_ptr<const std::string> navigation_mesh_export;
  std::uint64_t phase9_hints_control_calls = 0U;
  std::uint64_t phase9_hints_talk_calls = 0U;
  std::uint32_t phase9_hints_actor = 0U;
  std::uint64_t phase9_king_bear_control_calls = 0U;
  std::uint32_t phase9_king_bear_actor = 0U;
  std::uint32_t phase9_king_bear_callback = 0U;
  std::uint64_t phase9_hinttext_start_calls = 0U;
  std::uint64_t phase9_hinttext_stop_calls = 0U;
  std::uint64_t phase9_hinttext_active_calls = 0U;
  std::uint64_t phase9_hinttext_active_zero_returns = 0U;
  std::uint64_t phase9_hinttext_active_nonzero_returns = 0U;
  std::uint64_t phase9_hinttext_update_calls = 0U;
  std::uint64_t phase9_hinttext_advance_calls = 0U;
  std::uint64_t phase9_hinttext_accept_calls = 0U;
  std::uint32_t phase9_hinttext_active_last_return = 0U;
  std::uint64_t interrupt_timeslices = 0U;
  std::uint64_t rsp_yield_requests = 0U, rsp_yield_queries = 0U;
  std::unordered_map<std::uint32_t, std::uint32_t> empty_receive_polls;
  std::uint64_t receive_successes = 0U;
  std::uint64_t receive_empties = 0U;
  std::uint64_t receive_blocks = 0U;
  std::uint32_t last_receive_queue = 0U;
  std::uint32_t last_receive_block = 0U;
  std::int32_t last_receive_result = 0;
  std::uint64_t queue_create_calls = 0U;
  std::uint64_t queue_send_calls = 0U;
  std::uint64_t queue_recv_calls = 0U;
  std::uint64_t sp_task_load_calls = 0U;
  std::uint64_t sp_task_start_calls = 0U;
  std::uint64_t controller_read_start_calls = 0U;
  std::uint64_t controller_get_data_calls = 0U;
  std::uint64_t vi_swap_calls = 0U;
  std::uint64_t graphics_tasks = 0U;
  std::uint64_t graphics_commands = 0U;
  std::uint64_t graphics_triangles = 0U;
  std::uint64_t presented_frames = 0U;
  bool frame_captured = false;
  std::uint64_t controller_samples = 0U;
  std::uint64_t non_neutral_controller_samples = 0U;
  std::uint64_t non_neutral_controller_writes = 0U;
  std::uint64_t controller_disconnects = 0U;
  std::uint64_t controller_reconnects = 0U;
  std::uint64_t game_pressed_observations = 0U;
  std::uint16_t last_game_pressed_buttons = 0U;
  std::uint64_t game_stick_observations = 0U;
  std::int8_t last_game_stick_x = 0;
  std::int8_t last_game_stick_y = 0;
  std::uint64_t menu_stick_observations = 0U;
  std::int8_t last_menu_stick_x = 0;
  std::int8_t last_menu_stick_y = 0;
  std::size_t input_replay_event_count = 0U;
  std::uint64_t flash_persist_count = 0U;
  bool flash_image_loaded = false;
  bool flash_image_created = false;
  std::uint32_t active_bzero_address = 0U;
  std::uint32_t active_bzero_length = 0U;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  jfg::FlashRamStore flashram;
  bool fast_replay = false;
  bool realtime_replay = false;
  bool exit_requested = false;
  bool pc_experimental = false;
  std::array<jfg::PcInputConfig,4> pc_input{};
  std::array<jfg::ControllerReplaySample,4> pc_frames{};
  std::array<bool,4> pc_mouse_used{};
  bool pc_record_extended = false;
  std::array<jfg::PcOrbit,4> pc_orbits{};
  std::uint32_t pc_camera_actor=0,pc_camera_control=0,pc_camera_stack=0;
  jfg::PcLookDelta pc_camera_look{};
  std::uint64_t pc_aim_calls = 0, pc_direct_aim_calls = 0;
  std::array<jfg::N64StickSample,4> pc_aim{};
  std::array<int,4> pc_devices{-2,-2,-2,-2};
  jfg::PcWheelPulses pc_wheel;
  jfg::PcMouseMotion pc_mouse;
  // RGO direct aim: keyboard aim keys and hold/toggle aim.
  jfg::PcKeyAim pc_key_aim;
  std::chrono::steady_clock::time_point pc_key_aim_time{};
  std::array<jfg::PcAimLatch,4> pc_aim_latch{};
  // RGO: direct angle deltas that add to the look channel (gyro), applied
  // by the aim and camera hooks once per poll. Not part of input recordings.
  std::array<jfg::PcLookDelta,4> pc_direct{};
  std::array<bool,4> pc_direct_used{};
  std::array<jfg::PcGyroAim,4> pc_gyro_aim{};
  // RGO dual input: Player 1's second device (companion_device), which input
  // paths are live per port this poll, and the controller slot per port.
  int pc_companion = -2;
  std::array<bool,4> pc_keyboard_active{}, pc_pad_active{};
  std::array<int,4> pc_pad_slot{-2,-2,-2,-2};
  jfg::LastUsedDevice pc_last_used;
  int pc_wheel_key = 0;
  std::atomic<bool> pc_mouse_captured{false};
  bool pc_input_blocked = true, pc_raw_available = false;
  jfg::WindowInputMailbox window_input;
#if defined(_WIN32) || defined(JFG_HEADLESS_LIVE)
  // RGO: SDL gamepads (DualShock 4, DualSense, Switch Pro, Xbox) and rumble.
  // The headless build runs them too, so its timing includes the input thread.
  std::unique_ptr<jfg::SdlGamepads> gamepads;
  bool gamepads_failed = false, rumble_blocked = false;
  jfg::GamepadSnapshot gamepad_snapshot;
  jfg::RumbleModel rumble;
#endif
  std::array<bool, 256U> host_keys{};
  std::array<bool, 256U> host_key_presses{};
  std::uint16_t latched_controller_buttons = 0U;
  std::int8_t latched_controller_stick_x = 0;
  std::int8_t latched_controller_stick_y = 0;
  bool latched_controller_connected = true;
  jfg::DeterministicInputReplay input_replay;
  bool input_replay_loaded = false;
  bool input_replay_by_poll = false;
  std::string frame_capture_path;
  std::string rdram_capture_path;
  std::string progress_path;
  std::string input_record_path;
  std::string retrace_hash_path;
  jfg::AsyncDiagnosticFile input_record;
  std::ofstream phase95_poll_trace;
  std::ofstream retrace_hash_trace;
  std::ofstream update_hash_trace;
  std::ofstream update_word_trace;
  std::uint32_t update_word_address = 0U;
  std::ofstream controller_return_trace;
  std::uint32_t controller_return_rows = 0U;
  std::ofstream poll_hash_trace;
  std::ofstream event_trace;
  std::array<std::pair<std::uint64_t, std::uint64_t>, 2U> event_ranges{};
  std::size_t event_range_count = 0U;
  std::uint64_t event_trace_rows = 0U;
  std::uint64_t completed_game_updates = 0U;
  std::uint64_t focus_update_start = 0U;
  std::uint64_t focus_update_end = 0U;
  std::uint32_t watch_word_address = 0U;
  std::ofstream watch_word_trace;
  std::uint32_t watch_dispatch_depth = 0U;
  std::uint32_t watch_dispatch_changes = 0U;
  std::uint32_t entry_probe_target = 0U;
  std::ofstream entry_probe_trace;
  std::uint32_t entry_probe_hits = 0U;
  std::ofstream entry_fpu_trace;
  std::ofstream entry_memory_trace;
  std::ofstream entry_gpr_trace;
  std::ofstream instruction_probe_trace;
  std::uint32_t instruction_probe_hits = 0U;
  std::vector<std::uint32_t> point_probe_pcs;
  std::vector<std::uint32_t> point_probe_words;
  std::ofstream point_probe_trace;
  std::uint32_t point_probe_hits = 0U;
  jfg::AsyncDiagnosticFile timing_trace;
  jfg::AsyncBoundedTrace reticle_trace;
  bool reticle_trace_started = false, reticle_trace_active = false, reticle_trace_enabled = false;
  std::uint32_t reticle_actor = 0U, reticle_control = 0U;
  GameplayTrace gameplay_trace;
  std::uint64_t gameplay_trace_vi = UINT64_MAX;
  std::uint64_t gameplay_trace_updates = 0U;
  std::array<std::array<std::uint64_t, 56>, 256> gameplay_actor_previous{};
  std::array<bool, 256> gameplay_actor_seen{};
  bool actor_timing_trace = false;
  std::uint64_t last_actor_trace_retrace =
      (std::numeric_limits<std::uint64_t>::max)();
  bool host_frame_start_initialized = false;
  std::chrono::steady_clock::time_point host_frame_start{};
  bool present_time_initialized = false;
  std::chrono::steady_clock::time_point present_time{};
  std::uint64_t host_frame_interval_max_us = 0U;
  std::uint64_t present_interval_max_us = 0U;
  std::uint64_t graphics_prepare_max_us = 0U;
  std::uint64_t graphics_submit_max_us = 0U;
  std::uint64_t graphics_present_max_us = 0U;
  std::uint64_t host_frames_over_25ms = 0U;
  std::uint64_t host_frames_over_50ms = 0U;
  std::uint64_t host_frames_over_100ms = 0U;
  std::uint64_t last_progress_retrace =
      (std::numeric_limits<std::uint64_t>::max)();
  std::filesystem::path flash_path;
  std::filesystem::path controller_pak_path;
  std::unique_ptr<jfg::ControllerAccessoryBus> controller_accessory_bus;
  jfg::ControllerPakSession controller_pak_session;
  bool controller_pak_loaded = false;
  bool controller_pak_created = false;
  std::array<LiveViFieldRegisters, 2U> vi_fields{};
  std::uint32_t vi_mode_horizontal_start = 0x006C02ECU;
  bool vi_mode_configured = false;
  bool host_frame_deadline_initialized = false;
  std::chrono::steady_clock::time_point host_frame_deadline{};
  jfg::Rt64ViRegisters rt64_vi;
  std::uint32_t vi_mode_control = 0U;
  void *native_window = nullptr;
  jfg::ViPresentationSize window_presentation{320U, 240U};
  // RGO widescreen and display settings. The launcher passes its profile's
  // video.ini through JFG_VIDEO_CONFIG; edits apply live while playing.
  bool video_settings_checked = false;
  // False for replays and captures without a launcher: the renderer then
  // keeps its original resolution and VSync behaviour.
  bool video_managed = false;
  jfg::VideoSettings video{jfg::AspectMode::original};
  bool widescreen = false;
  // FPS counter: game frames and swap-chain presents (interpolated frames
  // included) counted since the last report to the frontend.
  std::uint64_t frame_rate_presents = 0U;
  std::uint64_t frame_rate_display_start = 0U;
  std::chrono::steady_clock::time_point frame_rate_start{};
  // FPS counter, Detailed (jfg/renderer/frame_stats.hpp): game logic
  // updates and their VI ticks, drawn/new/camera/repeated presents and the
  // gaps between game presents, all since the last report.
  std::uint64_t frame_logic_ticks = 0U;
  std::uint64_t frame_drawn = 0U;
  std::uint64_t frame_new = 0U;
  std::uint64_t frame_camera = 0U;
  std::uint64_t frame_repeats = 0U;
  std::uint64_t frame_input_start = 0U;
  jfg::IntervalRange frame_gaps{};
  jfg::FrameContentTracker frame_content{};
  // RGO frame interpolation: the VI count at the previous game frame, and
  // how many object matrices each object pushed since the last camera.
  std::uint64_t last_frame_retrace = 0U;
  // True 60: texture scroller ticks waiting to run in pairs, and whether the
  // previous frame ran at true 60 (to hand the game back its 30 fps mode).
  std::uint32_t scroller_pending_ticks = 0U;
  // True 60: the half-rate functions (the bug AI and other per-call code)
  // run once per 2 ticks. Ticks waiting to run, and the rate they run with
  // this frame (0: skipped this frame).
  std::uint32_t half_rate_pending_ticks = 0U;
  std::uint32_t half_rate_ticks = 0U;
  std::uint64_t squaddie_calls = 0U;  // headless counter log: bugs are active
  bool true60_active = false;
  jfg::True60Governor true60_governor;
  std::uint32_t true60_level = 0U;  // the governor's scene (kCurrentLevel)
  // RGO host busy time per game frame (the governor's evidence and the
  // Detailed counter's "host" line): wall time between two viFrameSync calls
  // minus the time the emulation thread slept waiting for a VI.
  std::uint64_t pacing_wait_us = 0U;      // all VI pacing sleeps so far
  std::uint64_t busy_frame_wait_us = 0U;  // pacing_wait_us at the last frame
  std::chrono::steady_clock::time_point busy_frame_start{};
  std::uint64_t frame_busy_us = 0U, frame_busy_max_us = 0U, frame_busy_frames = 0U;
  std::uint32_t frame_busy_last_us = 0U;  // the frame that just ended
  std::uint64_t vi_wake_late_max_us = 0U;  // latest VI pacing wake-up, this report
  // The runtime counters at the start of this report (Detailed counter).
  std::uint64_t frame_mmio_start = 0U, frame_mmio_stepped_start = 0U;
  std::uint64_t frame_switch_start = 0U, frame_wake_start = 0U;
  std::uint64_t frame_graphics_start = 0U, frame_submit_us_start = 0U;
  // RGO fast clock real-time floor: the Count of the last paced VI and the
  // wall time it was due, so Count can follow the wall clock when the host
  // falls behind (JFG_RGO_DETERMINISTIC_CLOCK=1 turns this off).
  std::uint64_t wall_anchor_count = 0U;
  std::chrono::steady_clock::time_point wall_anchor_time{};
  bool wall_anchor_valid = false;
  bool wall_floor_disabled = false;
  bool fast_run_disabled = false;  // JFG_RGO_NO_FAST_RUNS: check every instruction
  // Headless JFG_RGO_UNPACED=1: VIs do not wait for the wall clock, so a
  // deterministic run measures how fast the host can run the game.
  bool unpaced = false;
  std::uint32_t wall_floor_calls = 0U;
  std::uint64_t wall_floor_jumps = 0U;
  std::uint64_t wall_floor_resets = 0U;
  // RGO fast clock: recent empty NOBLOCK receives per call site and queue,
  // to recognize a polling wait.
  struct EmptyPollSite {
    std::uint32_t queue = 0U;
    std::uint32_t caller = 0U;
    std::uint64_t last_count = 0U;
    std::uint32_t streak = 0U;
  };
  std::array<EmptyPollSite, 8> empty_poll_sites{};
  std::uint64_t empty_poll_skips = 0U;
  // RGO FPS counter: game logic updates (viFrameSync calls).
  std::uint64_t logic_frames = 0U;
  std::uint64_t frame_rate_logic_start = 0U;
  bool counter_log = false;  // headless: print the counter to stderr
  // RGO frame trace (JFG_RGO_FRAME_TRACE): one row per game frame.
  std::FILE *rgo_frame_trace = nullptr;
  std::chrono::steady_clock::time_point rgo_frame_trace_start{};
#if defined(JFG_HEADLESS_LIVE)
  // RGO headless measurement: new game without a player (JFG_RGO_AUTOPILOT).
  std::optional<jfg::rgo::NewGameAutopilot> autopilot;
  std::uint8_t autopilot_front_mode = 0xFFU;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> autopilot_overlays;
#endif
  std::unordered_map<std::uint32_t, std::uint32_t> object_matrix_pushes;
  float widescreen_extent = 1.0F;
  // HUD anchoring is active for the current frontUpdate frame.
  bool hud_anchoring = false;
  jfg::mod::WidescreenHud hud;
  std::chrono::steady_clock::time_point video_settings_next_read{};
#if defined(_WIN32) || defined(JFG_HEADLESS_LIVE)
  std::unique_ptr<jfg::AsyncVolumeFile> video_settings_file;
#endif
  std::unique_ptr<jfg::Rt64Shell> rt64_shell;
  std::vector<std::byte> rt64_rdram;
  bool present_full_import = false;  // JFG_RGO_PRESENT_FULL_IMPORT=1
  // RGO audio queue-level trim of the VI deadlines (JFG_RGO_AUDIO_TRIM=0
  // turns it off) and the net trim so far (positive: ran ahead).
  bool audio_level_trim = true;
  std::int64_t audio_trim_ns = 0;
  std::uint32_t graphics_color_image = 0U;
  bool graphics_frame_ready = false;
  std::array<std::uint32_t, 64U> recent_graphics_color_images{};
  std::array<std::uint32_t, 64U> recent_graphics_command_addresses{};
  std::array<std::uint32_t, 64U> recent_graphics_vi_selections{};
  std::array<std::uint32_t, 64U> recent_graphics_matrix_commands{};
  std::array<std::uint32_t, 64U> recent_graphics_vertices_loaded{};
  std::array<std::uint32_t, 64U> recent_graphics_vertices_in_clip{};
  std::array<std::uint32_t, 64U> recent_graphics_triangles{};
  std::size_t recent_graphics_position = 0U;
  std::vector<PendingLiveGraphicsTask> pending_graphics_tasks;
  std::vector<LiveGraphicsOverlayShadow> graphics_overlay_shadows;
  // Immutable ROM-derived overlay shadows. Restore these bytes for each task;
  // display-list traversal still patches that task's private snapshot.
  std::vector<std::byte> graphics_overlay_initial_data;
  std::vector<PooledGraphicsSnapshot> graphics_snapshot_pool;
  // RGO host speed (snapshot delta import): the id of the snapshot the
  // renderer's RAM copy holds (0: unknown), its upper (overlay) half as
  // imported, chunks changed in the renderer's copy since (present windows),
  // JFG_RGO_SNAPSHOT_FULL=1 (always import fully) and JFG_RGO_SNAPSHOT_VERIFY=1
  // (replay every import into a model of the renderer's copy and compare).
  std::uint64_t next_snapshot_id = 0U;
  std::uint64_t imported_snapshot_id = 0U;
  std::vector<std::byte> imported_upper;
  std::vector<std::uint32_t> import_extra_chunks;
  bool snapshot_full_import = false;
  bool snapshot_verify = false;
  std::vector<std::byte> snapshot_verify_model;
  std::uint64_t snapshot_delta_imports = 0U, snapshot_full_imports = 0U;
  std::uint64_t snapshot_delta_chunks = 0U, snapshot_verify_mismatches = 0U;
  HostAudioDevice host_audio;
  std::uint64_t audio_task_microseconds = 0U;
  std::uint64_t graphics_prepare_microseconds = 0U;
  std::uint64_t graphics_submit_microseconds = 0U;
  std::uint64_t graphics_present_microseconds = 0U;
  std::array<std::uint32_t, 64U> recent_matrix_conversion_sources{};
  std::array<std::uint32_t, 64U> recent_matrix_conversion_destinations{};
  std::array<std::array<std::uint32_t, 4U>, 64U>
      recent_matrix_conversion_first_rows{};
  std::array<std::array<std::uint32_t, 4U>, 64U>
      recent_matrix_conversion_output_words{};
  std::size_t recent_matrix_conversion_position = 0U;
  std::array<std::array<std::uint32_t, 3U>, 64U> recent_matrix_cat_args{};
  std::array<std::array<std::uint32_t, 9U>, 64U>
      recent_matrix_cat_words{};
  std::size_t recent_matrix_cat_position = 0U;
  std::array<std::uint32_t, 8U> matrix_watch_addresses{
      0x801FDA80U, 0x801FDAC0U, 0x802239A0U, 0x802239E0U,
      0x800FAAF0U, 0x800FAF7CU};
  std::array<std::uint32_t, 8U> matrix_watch_words{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_addresses{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_before{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_after{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_targets{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_callers{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_current_targets{};
  std::array<std::uint32_t, 64U> recent_matrix_watch_return_addresses{};
  std::size_t recent_matrix_watch_position = 0U;
  std::array<std::uint32_t, 6U> poison_matrix_watch{};
  std::array<std::uint32_t, 16U> poison_dispatch_history{};
  jfg::Rt64ShellError last_rt64_error = jfg::Rt64ShellError::none;
  std::array<std::uint32_t, 6U> last_graphics_descriptor{};
  std::uint32_t last_graphics_command_address = 0U;
  std::uint32_t last_graphics_command_word = 0U;
  std::uint32_t materialize_reject_reason = 0U;
  std::uint32_t materialize_reject_address = 0U;
  std::uint32_t materialize_reject_word0 = 0U;
  std::uint32_t materialize_reject_word1 = 0U;
  std::uint32_t materialize_parent_address = 0U;
  std::uint32_t materialize_parent_word0 = 0U;
  std::uint32_t materialize_parent_word1 = 0U;
  std::uint32_t materialize_block_address = 0U;
  std::uint32_t materialize_parent_segment_base = 0U;
#endif
  std::array<std::uint64_t, kMappingCount> mapped_call_counts{};
  std::uint32_t loaded_rsp_task = 0U;
  std::uint32_t loaded_rsp_task_type = 0U;
  std::uint64_t decoded_audio_tasks = 0U;
  bool audio_device_initialized = false;
  bool audio_device_started = false;
  std::uint32_t audio_device_frequency = 0U;
  std::uint64_t audio_device_buffers = 0U;
  std::uint64_t audio_device_queued_bytes = 0U;
  std::uint64_t audio_device_consumed_bytes = 0U;
  std::uint64_t audio_device_consumed_ms = 0U;
  std::uint64_t audio_device_underruns = 0U;
  std::uint64_t audio_device_overruns = 0U;
  int controller_read_thread = -1;
  std::uint64_t si_receive_calls = 0U;
  std::uint32_t last_si_receive_return = 0U;
  std::uint32_t last_si_receive_block = 0U;
  std::array<std::uint32_t, 32U> recent_dispatches{};
  std::size_t recent_dispatch_position = 0U;
  std::array<std::uint32_t, 16U> last_si_dispatch_trace{};
};

#if defined(JFG_LIVE_RUNTIME)
State *g_active_child_state = nullptr;

#if defined(_WIN32)
class LiveRt64Window final {
public:
  static constexpr UINT kAspectMessage = WM_APP + 22U;
  LiveRt64Window(State &state, const bool visible) : input_(state.window_input), captured_(state.pc_mouse_captured) {
    // No guest state is accessed by this thread. A window move/resize can enter
    // Win32's modal loop for arbitrarily long without parking the guest CPU.
    std::promise<void> ready;
    auto initialized = ready.get_future();
    owner_ = std::thread([this, visible, &ready] {
      thread_id_ = GetCurrentThreadId();
      instance_ = GetModuleHandleW(nullptr);
      WNDCLASSW window_class{};
      window_class.lpfnWndProc = window_proc;
      window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
      window_class.hInstance = instance_;
      window_class.lpszClassName = kClassName;
      atom_ = RegisterClassW(&window_class);
      if (atom_ != 0U) {
        wchar_t parent_text[32]{};
        HWND parent=nullptr;
        if(GetEnvironmentVariableW(L"JFG_FRONTEND_PARENT",parent_text,32)>0) {
          wchar_t* end=nullptr;const auto value=wcstoull(parent_text,&end,10);
          parent=reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
          if(!end || *end || !IsWindow(parent) || !GetPropW(GetAncestor(parent,GA_ROOT),L"JfgFrontend")) {
            ready.set_value();UnregisterClassW(kClassName,instance_);return;
          }
        }
        const DWORD style=parent ? WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS : WS_OVERLAPPEDWINDOW;
        RECT rectangle{0,0,640,480};
        if(parent)GetClientRect(parent,&rectangle);
        if (parent || AdjustWindowRectEx(&rectangle,style,FALSE,0U)) {
          window_ = CreateWindowExW(0U,kClassName,L"Jet Force Gemini Recomp",style,
              parent?0:CW_USEDEFAULT,parent?0:CW_USEDEFAULT,rectangle.right-rectangle.left,
              rectangle.bottom-rectangle.top,parent,nullptr,instance_,this);
        }
        if(window_) {
          RAWINPUTDEVICE mouse{0x01,0x02,0,window_};
          raw_available_=RegisterRawInputDevices(&mouse,1,sizeof(mouse))!=FALSE;
          if(parent)PostMessageW(GetAncestor(parent,GA_ROOT),WM_APP+20,reinterpret_cast<WPARAM>(window_),0);
          if(visible){ShowWindow(window_,SW_SHOW);UpdateWindow(window_);}
        }
      }
      ready.set_value();
      if (window_) {
        MSG message{};
        while (GetMessageW(&message,nullptr,0U,0U)>0) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        ClipCursor(nullptr);
        DestroyWindow(window_);
      }
      if (atom_) UnregisterClassW(kClassName,instance_);
    });
    initialized.get(); // Publishes the initialized HWND and owner thread ID.
    state.native_window = window_;
    state.pc_raw_available = raw_available_;
  }
  ~LiveRt64Window() {
    if (window_) PostMessageW(window_,kStopMessage,0,0);
    if (owner_.joinable()) owner_.join();
  }
  LiveRt64Window(const LiveRt64Window&) = delete;
  LiveRt64Window& operator=(const LiveRt64Window&) = delete;
  [[nodiscard]] HWND get() const noexcept { return window_; }
  [[nodiscard]] DWORD thread_id() const noexcept { return thread_id_; }
private:
  static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto *create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto *self = reinterpret_cast<LiveRt64Window*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (!self) return DefWindowProcW(window,message,wparam,lparam);
    if(message==WM_INPUT) {
      RAWINPUT raw{};UINT size=sizeof(raw);
      if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))!=UINT(-1) &&
         raw.header.dwType==RIM_TYPEMOUSE && !(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE) && self->captured_.load())
        self->input_.motion(raw.data.mouse.lLastX,raw.data.mouse.lLastY);
    }
    if(message==WM_SETCURSOR && self->captured_.load()){SetCursor(nullptr);return TRUE;}
    if(message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) {
      int key=0;bool down=false;
      switch(message) {
      case WM_LBUTTONDOWN:case WM_LBUTTONUP:key=VK_LBUTTON;down=message==WM_LBUTTONDOWN;break;
      case WM_RBUTTONDOWN:case WM_RBUTTONUP:key=VK_RBUTTON;down=message==WM_RBUTTONDOWN;break;
      case WM_MBUTTONDOWN:case WM_MBUTTONUP:key=VK_MBUTTON;down=message==WM_MBUTTONDOWN;break;
      case WM_XBUTTONDOWN:case WM_XBUTTONUP:key=HIWORD(wparam)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2;down=message==WM_XBUTTONDOWN;break;
      case WM_MOUSEWHEEL:self->input_.wheel(static_cast<short>(HIWORD(wparam)));break;
      default:break;
      }
      if(key) {
        if(down){SetFocus(window);SetCapture(window);}
        self->buttons_[static_cast<std::size_t>(key)]=down;self->input_.key(static_cast<std::size_t>(key),down);
        if(!down && GetCapture()==window && !self->buttons_[VK_LBUTTON] && !self->buttons_[VK_RBUTTON] &&
            !self->buttons_[VK_MBUTTON] && !self->buttons_[VK_XBUTTON1] && !self->buttons_[VK_XBUTTON2])ReleaseCapture();
        return message==WM_XBUTTONDOWN || message==WM_XBUTTONUP?TRUE:0;
      }
    }
    if(message==WM_CAPTURECHANGED)for(auto key:{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2}){
      self->buttons_[static_cast<std::size_t>(key)]=false;self->input_.key(static_cast<std::size_t>(key),false);
    }
    if ((message == WM_KEYDOWN || message == WM_KEYUP) && wparam < 256U) {
      if(GetParent(window) && message==WM_KEYDOWN && (wparam==VK_F11 || wparam==VK_ESCAPE)) {
        PostMessageW(GetAncestor(window,GA_ROOT),WM_APP+21,wparam,0);return 0;
      }
      self->input_.key(static_cast<std::size_t>(wparam),message == WM_KEYDOWN);
      return 0;
    }
    if (message == WM_KILLFOCUS) {
      self->captured_.store(false);self->buttons_.fill(false);
      if(GetCapture()==window)ReleaseCapture();ClipCursor(nullptr);
      self->input_.lose_focus();return 0;
    }
    if (message == WM_CLOSE) { self->input_.close(); return 0; }
    if (message == kStopMessage) { PostQuitMessage(0); return 0; }
    if (message == kAspectMessage) {
      RECT client{};
      if (!GetParent(window) && wparam != 0U && lparam > 0 && !IsZoomed(window) && !IsIconic(window) &&
          GetClientRect(window,&client) && client.bottom > client.top) {
        const LONG height = client.bottom-client.top;
        const LONG width = static_cast<LONG>((std::uint64_t(height)*wparam+
            static_cast<std::uint64_t>(lparam)/2U)/static_cast<std::uint64_t>(lparam));
        RECT outer{0,0,width,height};
        if (AdjustWindowRectEx(&outer,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),
            FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE))))
          SetWindowPos(window,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,
              SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
      }
      return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
  }
  static constexpr UINT kStopMessage = WM_APP+23U;
  static constexpr wchar_t kClassName[] = L"JfgPhase8LiveRt64Window";
  jfg::WindowInputMailbox& input_;
  std::atomic<bool>& captured_;
  std::array<bool,256> buttons_{};
  bool raw_available_ = false;
  HINSTANCE instance_ = nullptr;
  ATOM atom_ = 0U;
  HWND window_ = nullptr;
  DWORD thread_id_ = 0;
  std::thread owner_;
};
#else
// Headless: no window. Input comes from replays; the frontend is absent.
class LiveRt64Window final {
public:
  static constexpr UINT kAspectMessage = WM_APP + 22U;
  LiveRt64Window(State &, bool) noexcept {}
  [[nodiscard]] HWND get() const noexcept { return nullptr; }
  [[nodiscard]] DWORD thread_id() const noexcept { return 0U; }
};
#endif

bool host_key_down(const State &state, const int virtual_key) noexcept {
  return virtual_key >= 0 && virtual_key < 256 &&
         (state.host_keys[static_cast<std::size_t>(virtual_key)] ||
          state.host_key_presses[static_cast<std::size_t>(virtual_key)]);
}

void trace_phase9_event(State &state, const char *event,
                        const std::uint64_t poll) {
  if (!state.event_trace.is_open())
    return;
  bool selected = false;
  for (std::size_t index = 0U; index < state.event_range_count; ++index) {
    const auto [first, last] = state.event_ranges[index];
    selected |= poll >= first && poll <= last;
  }
  if (!selected)
    return;
  if (++state.event_trace_rows > 4096U) {
    state.exit_requested = true;
    return;
  }
  state.event_trace << state.event_trace_rows << '\t' << event << '\t'
                    << poll << '\t' << state.completed_game_updates << '\t'
                    << state.vi_frames << '\t' << state.vi_retraces << '\t'
                    << state.controller_read_start_calls << '\t'
                    << state.controller_get_data_calls << '\t'
                    << (state.latched_controller_connected ? 1 : 0) << '\t'
                    << state.latched_controller_buttons << '\t'
                    << static_cast<int>(state.latched_controller_stick_x) << '\t'
                    << static_cast<int>(state.latched_controller_stick_y) << '\n';
}

#if defined(_WIN32)
bool read_xinput_device(int device,
                        jfg::StandardControllerSample &sample) noexcept {
  using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
  static XInputGetStateFn get_state = []() noexcept -> XInputGetStateFn {
    constexpr const wchar_t *kLibraries[] = {L"xinput1_4.dll", L"xinput1_3.dll",
                                             L"xinput9_1_0.dll"};
    for (const wchar_t *library : kLibraries) {
      HMODULE module = LoadLibraryW(library);
      if (module == nullptr)
        continue;
      auto *proc = GetProcAddress(module, "XInputGetState");
      if (proc != nullptr)
        return reinterpret_cast<XInputGetStateFn>(proc);
    }
    return nullptr;
  }();
  if (get_state == nullptr)
    return false;
  if (device < 0 || device >= 4)
    return false;
  XINPUT_STATE state{};
  bool connected = false;
  static std::array<ULONGLONG, 4> retry_after{};
  const auto now = GetTickCount64();
  for (DWORD port = 0U; port < 4U; ++port) {
    if (now < retry_after[port])
      continue;
    if (port != static_cast<DWORD>(device))
      continue;
    if (get_state(port, &state) == ERROR_SUCCESS) {
      connected = true;
      break;
    }
    retry_after[port] = now + 500;
  }
  if (!connected)
    return false;
  constexpr std::array<WORD, 15> masks{0x1000, 0x2000, 0x4000, 0x8000, 0x20,
                                       0,      0x10,   0x40,   0x80,   0x100,
                                       0x200,  1,      2,      4,      8};
  for (std::size_t i = 0U; i < masks.size(); ++i)
    sample.buttons[i] = (state.Gamepad.wButtons & masks[i]) != 0;
  sample.axes = {state.Gamepad.sThumbLX,
                 -static_cast<int>(state.Gamepad.sThumbLY),
                 state.Gamepad.sThumbRX,
                 -static_cast<int>(state.Gamepad.sThumbRY),
                 state.Gamepad.bLeftTrigger * 32767 / 255,
                 state.Gamepad.bRightTrigger * 32767 / 255};
  return true;
}
#else
bool read_xinput_device(int, jfg::StandardControllerSample &) noexcept {
  return false;
}
#endif

// RGO: gamepads come from SDL's GameController API on its own input thread.
// XInput remains the fallback if SDL's controller support cannot start.
bool read_gamepads(State &state) {
  if (!state.gamepads && !state.gamepads_failed) {
    state.gamepads = std::make_unique<jfg::SdlGamepads>();
    if (!state.gamepads->start()) {
      state.gamepads.reset();
      state.gamepads_failed = true;
    }
  }
  if (!state.gamepads)
    return false;
  state.gamepad_snapshot = state.gamepads->snapshot();
  return true;
}
void stop_rumble(State &state) {
  state.rumble.stop_all();
  if (state.gamepads)
    state.gamepads->stop_rumble();
}
double rumble_clock() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// RGO rumble: the game's rumble requests (rumbleStart/Alter/Max/Stop/Kill in
// saves.c) play on the player's controller. No Rumble Pak is emulated: the
// requests are observed before they run, with the game's own checks.
void rumble_request(State &state, const std::uint32_t target, const recomp_context &context) {
  jfg::mod::Memory ram({state.rdram, kRdramSize});
  constexpr std::uint32_t kRumbleStart = 0x8004B0F4U, kRumbleStop = 0x8004B204U,
      kRumbleAlter = 0x8004B29CU, kRumbleKill = 0x8004B518U;
  if (!state.gamepads || state.rumble_blocked ||
      ram.u32(target) != (target == kRumbleKill ? 0x27BDFFE0U : 0x27BDFFE8U))
    return;
  jfg::GameRumbleRequest request;
  if (target == kRumbleKill) {
    request.call = jfg::GameRumbleCall::kill;
  } else {
    const auto player = static_cast<std::int32_t>(context.r4);
    if (player < 0 || player >= 4)
      return;
    request.channel = ram.u8(0x800FB100U + static_cast<std::uint32_t>(player)); // joyGetController
    if (request.channel >= 4)
      return;
    const int phase = ram.u16(0x800FDF58U + 10U * static_cast<std::uint32_t>(request.channel)) >> 12;
    if (target == kRumbleStop) {
      request.call = jfg::GameRumbleCall::stop;
    } else {
      // func_8004B070: rumble is enabled and the game is not paused.
      if (ram.u8(0x800A3470U) == 0U || ram.u8(0x800FD7BDU) != 0U)
        return;
      request.call = target == kRumbleStart ? jfg::GameRumbleCall::start
                   : target == kRumbleAlter ? jfg::GameRumbleCall::alter
                                            : jfg::GameRumbleCall::max;
      request.strength = static_cast<std::int32_t>(context.r5);
      // The float third argument travels in a2 (o32: the first argument is an int).
      request.seconds = std::bit_cast<float>(static_cast<std::uint32_t>(context.r6));
      if (phase == 2) { // resting: only rumbleAlter's strength applies
        if (request.call != jfg::GameRumbleCall::alter)
          return;
        request.seconds = 0.0F;
      }
    }
  }
  const double now = rumble_clock();
  state.rumble.apply(request, now);
  for (int channel = 0; channel < 4; ++channel) {
    if (request.call != jfg::GameRumbleCall::kill && channel != request.channel)
      continue;
    const int slot = state.pc_pad_slot[static_cast<std::size_t>(channel)];
    if (slot < 0)
      continue;
    const auto out = state.rumble.output(channel, now, state.pc_input[static_cast<std::size_t>(channel)].rumble);
    state.gamepads->rumble(slot, out.amplitude, out.amplitude * 0.6F, out.milliseconds);
  }
}

// RGO: the direct angle deltas (gyro) for a port, once per poll.
jfg::PcLookDelta take_direct(State &state, std::size_t port) {
  if (port >= state.pc_direct.size() || state.pc_direct_used[port])
    return {};
  state.pc_direct_used[port] = true;
  return state.pc_direct[port];
}

void reload_controller_ports(State &state) {
  const auto now = std::chrono::steady_clock::now();
  if (now - state.controller_config_check < std::chrono::milliseconds(250))
    return;
  state.controller_config_check = now;
  if (state.controller_config_path.empty())
    return;
  std::string pc_optin;
  std::ifstream pc_switch(state.controller_config_path.parent_path() / "experimental-controls.ini",std::ios::binary|std::ios::ate);
  if(pc_switch && pc_switch.tellg()>0 && pc_switch.tellg()<=128) {
    pc_switch.seekg(0);pc_optin.assign(std::istreambuf_iterator<char>(pc_switch),{});
  }
  const bool pc_enabled=jfg::pc_experiments_enabled(pc_optin);
  if(pc_enabled!=state.pc_experimental) {
    state.pc_experimental=pc_enabled;state.pc_aim.fill({});
    state.pc_mouse.clear();state.pc_wheel.clear();state.pc_wheel_key=0;
    state.pc_mouse_captured=false;ClipCursor(nullptr);
    SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)));
    (void)state.controller_input_gate.blocked(true,state.controller_ports.samples);
  }
  for (std::size_t p = 0; p < 4; ++p) {
    const auto pc_path=state.controller_config_path.parent_path() /
      (p==0?"pc-input.ini":"pc-input-"+std::to_string(p+1)+".ini");
    std::ifstream pc_file(pc_path,std::ios::binary|std::ios::ate);
    if(pc_file && pc_file.tellg()>0 && pc_file.tellg()<=4096) {
      pc_file.seekg(0);const std::string text{std::istreambuf_iterator<char>(pc_file),{}};
      (void)jfg::parse_pc_input(text,state.pc_input[p]);
    }
    const auto path =
        p == 0 ? state.controller_config_path
               : state.controller_config_path.parent_path() /
                     ("controller-" + std::to_string(p + 1) + ".ini");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() > 4096)
      continue;
    file.seekg(0);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    jfg::ControllerMapping mapping;
    if (jfg::parse_controller_mapping(text, mapping))
      state.controller_ports.configure(p, mapping);
  }
}
bool pc_key_down(const State &state,int key) {
  return key>=256?state.pc_wheel_key==key:host_key_down(state,key);
}
jfg::ControllerPortSample sample_keyboard_port(State &state,std::size_t port,std::uint16_t *raw_buttons=nullptr) {
  const auto down=[&](int key){return pc_key_down(state,key);};
  const auto config=jfg::effective_pc_input(state.pc_experimental,state.pc_input[port]);
  auto sample=jfg::map_pc_keyboard(config,down);
  jfg::mod::Memory memory({state.rdram,kRdramSize});
  if(state.pc_experimental && (memory.u8(0x800FD7BDU)!=0 ||
     (memory.u8(0x800A4FC4U)==0 && !jfg::mod::gameplay_active(memory,state.phase9_player_actor))))
    sample=jfg::map_pc_menu(sample,down);
  // RGO: hold or toggle aim (R). Toggle only during gameplay; menus, pauses,
  // cutscenes and blocked input release it.
  const bool active=state.pc_experimental && !state.pc_input_blocked && memory.u8(0x800FD7BDU)==0 &&
    jfg::mod::gameplay_active(memory,state.phase9_player_actor) &&
    !jfg::mod::scripted_camera_active(memory,state.phase9_player_actor);
  if(raw_buttons)*raw_buttons=sample.buttons;
  const bool aim=state.pc_aim_latch[port].apply((sample.buttons&0x10U)!=0U,config.aim_toggle!=0,active);
  sample.buttons=static_cast<std::uint16_t>((sample.buttons&~0x10U)|(aim?0x10U:0U));
  return sample;
}
void sample_controller_ports(State &state) {
  reload_controller_ports(state);
  state.pc_wheel_key=state.pc_wheel.sample();
  state.pc_aim.fill({});
  state.pc_direct.fill({});
  std::array<bool, 4> available{};
  std::array<jfg::StandardControllerSample, 4> hardware{};
  if (read_gamepads(state)) {
    for (std::size_t p = 0; p < 4; ++p) {
      available[p] = state.gamepad_snapshot.info[p].connected;
      hardware[p] = state.gamepad_snapshot.pads[p];
    }
  } else
    for (std::size_t p = 0; p < 4; ++p)
      available[p] = read_xinput_device(static_cast<int>(p), hardware[p]);
  const auto devices = state.controller_ports.devices(available);
  state.pc_devices=devices;
  state.pc_companion=jfg::companion_device(devices,available);
  for (std::size_t p = 0; p < 4; ++p)
    state.pc_pad_slot[p] = devices[p] >= 0 ? devices[p] : p == 0 && state.pc_companion >= 0 ? state.pc_companion : -2;
  if (state.gamepads)
    for (std::size_t p = 0; p < 4; ++p)
      if (state.pc_pad_slot[p] >= 0)
        state.gamepads->light(state.pc_pad_slot[p], jfg::player_light(static_cast<int>(p)));
  jfg::mod::Memory pc_memory({state.rdram,kRdramSize});
  const bool look_active=state.pc_experimental &&
    jfg::mod::gameplay_active(pc_memory,state.phase9_player_actor) && pc_memory.u8(0x800FD7BDU)==0 &&
    !jfg::mod::scripted_camera_active(pc_memory,state.phase9_player_actor);
  // One controller's sample for player p, with its look stick and gyro.
  const auto pad_sample=[&](std::size_t p,int device)->jfg::ControllerPortSample {
    if (device < 0 || !available[static_cast<std::size_t>(device)])
      return {};
    const auto mapped=jfg::map_pc_controller(state.controller_ports.mappings[p],
        hardware[static_cast<std::size_t>(device)],state.pc_input[p],look_active);
    if(state.pc_experimental && state.pc_input[p].dual_stick)
      state.pc_aim[p]=jfg::pc_aim_stick(state.pc_input[p],hardware[static_cast<std::size_t>(device)],
        state.controller_ports.mappings[p].stick,!state.pc_input[p].modern || (mapped.buttons&0x10)!=0);
    // RGO gyro aim: rotation since the last poll becomes direct angle deltas,
    // while aiming or always, unless the ratchet binding is held. Input
    // recordings stay gyro-free so they replay exactly.
    if(state.gamepads) {
      const auto gyro=state.gamepads->take_gyro(device);
      const auto config=jfg::effective_pc_input(state.pc_experimental,state.pc_input[p]);
      const bool ratchet=jfg::binding_down(state.controller_ports.mappings[p],
        hardware[static_cast<std::size_t>(device)],config.gyro_ratchet);
      if(look_active && state.input_record_path.empty() &&
         jfg::pc_gyro_active(config,(mapped.buttons&0x10)!=0,ratchet))
        state.pc_direct[p]=jfg::pc_add_look(state.pc_direct[p],state.pc_gyro_aim[p].apply(config,gyro.yaw,gyro.pitch));
      else
        state.pc_gyro_aim[p].clear();
    }
    return {mapped.buttons, static_cast<std::int8_t>(mapped.stick.x),
            static_cast<std::int8_t>(mapped.stick.y), true};
  };
  for (std::size_t p = 0; p < 4; ++p) {
    auto &sample = state.controller_ports.samples[p];
    sample = {};
    const int device = devices[p];
    const bool keyboard = device == -3 || (p == 0 && state.pc_companion == -3);
    const int pad = state.pc_pad_slot[p];
    std::uint16_t raw_keys = 0;
    const auto keys = keyboard ? sample_keyboard_port(state,p,&raw_keys) : jfg::ControllerPortSample{};
    const auto pads = pad >= 0 ? pad_sample(p,pad) : jfg::ControllerPortSample{};
    state.pc_keyboard_active[p] = keyboard;
    state.pc_pad_active[p] = pad >= 0 && available[static_cast<std::size_t>(pad)];
    if (!keyboard || !state.pc_pad_active[p]) {
      sample = keyboard ? keys : pads;
      if (p == 0) state.pc_last_used.reset();
      continue;
    }
    // RGO dual input (Player 1 with a second device): both at once, or the
    // device used last. Gyro and toggled aim never count as use.
    if (state.pc_input[p].dual_input) {
      sample = jfg::merge_ports(keys, pads);
      continue;
    }
    const bool key_use = jfg::port_activity({raw_keys, keys.x, keys.y, true});
    const bool pad_use = jfg::port_activity(pads) || state.pc_aim[p].x || state.pc_aim[p].y;
    const bool primary_keyboard = device == -3;
    const bool companion = state.pc_last_used.update(primary_keyboard ? key_use : pad_use,
                                                     primary_keyboard ? pad_use : key_use);
    const bool use_keyboard = primary_keyboard != companion;
    sample = use_keyboard ? keys : pads;
    state.pc_keyboard_active[p] = use_keyboard;
    state.pc_pad_active[p] = !use_keyboard;
    if (use_keyboard) {
      state.pc_aim[p] = {};
      state.pc_direct[p] = {};
      state.pc_gyro_aim[p].clear();
    } else
      (void)state.pc_aim_latch[p].apply(false,false,false); // release a toggled aim
  }
  state.controller_mapping = state.controller_ports.mappings[0];
  state.controller_mapping.device = devices[0];
}

jfg::mod::DialogueState navigation_dialogue(State &state) {
  jfg::mod::Memory ram({state.rdram,kRdramSize});
  const auto base=section_addresses!=nullptr?static_cast<std::uint32_t>(section_addresses[7U]):0U;
  if(ram.valid(base,0x2C44U))return jfg::mod::read_dialogue(ram,base);
  JfgGeneratedSectionMetadata metadata{};
  if(!state.active_overlay_sections.contains(7U) ||
     !jfg_generated_section_metadata(7U,&metadata) || metadata.is_overlay!=1U ||
     base!=metadata.linked_vram || (base&3U))return {};
  const std::uint64_t extent=std::uint64_t(metadata.text_size)+metadata.data_size+metadata.bss_size;
  const std::uint32_t offset=base-kKseg0;
  if(extent<0x2C44U || extent>0x10000U || offset>kGuestAddressSpan ||
     extent>kGuestAddressSpan-offset || (extent&3U))return {};
  jfg::mod::DialogueMemory memory(ram,base,
      std::span<const std::uint8_t>(state.rdram+offset,static_cast<std::size_t>(extent)));
  return jfg::mod::read_dialogue(memory,base);
}

class SlowHostWork {
 public:
  SlowHostWork(State &state, const char *label, bool selected = true)
      : state_(state), label_(label),
        enabled_(selected && state.timing_trace.is_open()),
        start_(enabled_ ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{}) {}
  ~SlowHostWork() {
    if (!enabled_) return;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_).count();
    if (us >= 4000)
      state_.timing_trace << label_ << '\t' << state_.vi_frames << '\t'
                          << state_.vi_retraces << '\t' << us << '\n';
  }
 private:
  State &state_;
  const char *label_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
};


void sample_live_controller(State &state) noexcept {
  state.window_input.consume(state.host_keys,state.host_key_presses,state.exit_requested);
  {
    const auto input=state.window_input.take_motion();
    if(input.focus_lost){state.pc_mouse.clear();state.pc_wheel.clear();state.pc_input_blocked=true;}
    if(state.pc_mouse_captured.load())state.pc_mouse.add(input.x,input.y);
    state.pc_wheel.add(input.wheel);
  }
  const SlowHostWork timing(state, "host-controller");
  constexpr std::uint16_t kButtonA = 0x8000U;
  std::uint16_t buttons = 0U;
  int stick_x = 0;
  int stick_y = 0;
  if(!state.input_replay_loaded)sample_controller_ports(state);
  else {reload_controller_ports(state);state.pc_direct.fill({});}
  state.pc_frames.fill({});state.pc_mouse_used.fill(false);state.pc_direct_used.fill(false);
  bool connected = state.controller_ports.samples[0].connected;
  if (state.input_replay_loaded) {
    const jfg::ControllerReplaySample replay =
        state.input_replay_by_poll
            ? state.input_replay.sample_by_poll(state.controller_samples)
            : state.input_replay.sample_at(state.vi_retraces);
    if(state.pc_experimental)state.pc_frames[0]=replay;
    connected = replay.connected;
    buttons = replay.buttons;
    stick_x = replay.stick_x;
    stick_y = replay.stick_y;
  } else {
    const auto sample=state.controller_ports.samples[0];
    buttons=sample.buttons;stick_x=sample.x;stick_y=sample.y;
  }
#if defined(JFG_HEADLESS_LIVE)
  if (state.autopilot) {
    // KSEG0 RDRAM, plus overlay memory at linked addresses (recompiled
    // overlays live there), limited to the extents the backing mapped.
    struct GuestView final : jfg::rgo::AutopilotMemory {
      const State &state;
      explicit GuestView(const State &owner) : state(owner) {}
      const std::uint8_t *word(std::uint32_t address) const {
        if (address >= kKseg0 && address - kKseg0 <= kRdramSize - 4U)
          return state.rdram + (address - kKseg0);
        for (const auto &[begin, end] : state.autopilot_overlays)
          if (address >= begin && address <= end - 4U)
            return state.rdram + static_cast<std::uint32_t>(address + kKseg0);
        return nullptr;
      }
      std::optional<std::uint8_t> u8(std::uint32_t address) const override {
        const auto *bytes = word(address & ~3U);
        if (bytes == nullptr) return std::nullopt;
        return bytes[(address & 3U) ^ 3U];
      }
      std::optional<std::uint32_t> u32(std::uint32_t address) const override {
        if ((address & 3U) != 0U) return std::nullopt;
        const auto *bytes = word(address);
        if (bytes == nullptr) return std::nullopt;
        std::uint32_t value = 0U;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
      }
    } const rdram(state);
    const auto input = state.autopilot->sample(rdram);
    connected = true;
    buttons = input.buttons;
    stick_x = input.stick_x;
    stick_y = input.stick_y;
    const auto mode = rdram.u8(kKseg0 | jfg::rgo::kFrontModeAddress).value_or(0U);
    if (std::getenv("JFG_RGO_AUTOPILOT_DEBUG") != nullptr && (buttons != 0U || state.controller_samples % 30U == 0U)) {
      const auto keyboard = jfg::rgo::read_keyboard(rdram);
      std::fprintf(stderr, "autopilot: VI %llu poll %llu mode %u buttons %04x stick %d keyboard %d/%u/%u/%u game %08x\n",
                   static_cast<unsigned long long>(state.vi_retraces),
                   static_cast<unsigned long long>(state.controller_samples), mode, buttons, stick_x,
                   keyboard ? 1 : 0, keyboard ? keyboard->submode : 0U,
                   keyboard ? keyboard->key : 0U, keyboard ? keyboard->label_length : 0U,
                   rdram.u32(0x800FB0C0U).value_or(0U));
    }
    if (mode != state.autopilot_front_mode) {
      state.autopilot_front_mode = mode;
      std::fprintf(stderr, "autopilot: VI %llu front mode %u level %u\n",
                   static_cast<unsigned long long>(state.vi_retraces), mode,
                   rdram.u32(kKseg0 | jfg::rgo::kLevelNumberAddress).value_or(0U));
    }
  }
#endif

  const HWND root=state.native_window ? GetAncestor(static_cast<HWND>(state.native_window),GA_ROOT) : nullptr;
  const bool settings_capture=state.play_mode && root && GetPropW(root,L"JfgSettingsOpen")!=nullptr;
  const bool ui_capture=state.play_mode && root && (GetForegroundWindow()!=root || settings_capture);
  auto release_samples=state.controller_ports.samples;
  for(std::size_t p=0;p<4;++p)if(state.pc_aim[p].x || state.pc_aim[p].y)release_samples[p].buttons|=1;
  const bool block_physical=state.controller_input_gate.blocked(ui_capture,release_samples);
  state.pc_input_blocked=block_physical && !state.input_replay_loaded;
  // RGO: no rumble while settings are open or the game is in the background.
  if (block_physical && !state.rumble_blocked)
    stop_rumble(state);
  state.rumble_blocked = block_physical;
  jfg::mod::Memory pc_memory({state.rdram,kRdramSize});
  bool want_mouse=false;
  for(std::size_t p=0;p<4;++p)
    want_mouse|=state.pc_keyboard_active[p] && state.pc_input[p].mouse_aim && state.controller_ports.samples[p].connected &&
      (state.pc_input[p].modern || (state.controller_ports.samples[p].buttons&0x10)!=0);
  want_mouse=want_mouse && state.pc_experimental && state.pc_raw_available && !block_physical && !state.input_replay_loaded && (state.input_record_path.empty() || state.pc_record_extended) &&
    jfg::mod::gameplay_active(pc_memory,state.phase9_player_actor) && pc_memory.u8(0x800FD7BDU)==0 &&
    !jfg::mod::scripted_camera_active(pc_memory,state.phase9_player_actor);
  if(want_mouse && state.native_window) {
    const HWND window=static_cast<HWND>(state.native_window);RECT rect{};GetClientRect(window,&rect);
    MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&rect),2);ClipCursor(&rect);SetCursor(nullptr);
  } else {
    if(state.pc_mouse_captured){ClipCursor(nullptr);SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)));}
    state.pc_mouse.clear();
  }
  state.pc_mouse_captured=want_mouse;
  if(block_physical){state.pc_aim.fill({});state.pc_direct.fill({});state.pc_wheel.clear();state.pc_mouse.clear();state.pc_key_aim.clear();state.pc_orbits.fill({});}
  if(!state.pc_experimental || pc_memory.u8(0x800FD7BDU)!=0 || !jfg::mod::gameplay_active(pc_memory,state.phase9_player_actor) ||
     jfg::mod::scripted_camera_active(pc_memory,state.phase9_player_actor))state.pc_orbits.fill({});
  const auto key_aim_now=std::chrono::steady_clock::now();
  const double key_aim_seconds=state.pc_key_aim_time==std::chrono::steady_clock::time_point{}?0.0:
    std::chrono::duration<double>(key_aim_now-state.pc_key_aim_time).count();
  state.pc_key_aim_time=key_aim_now;
  if(state.pc_experimental && !state.input_replay_loaded && !block_physical &&
     (state.input_record_path.empty() || state.pc_record_extended)) {
    for(std::size_t p=0;p<4;++p) {
      auto &frame=state.pc_frames[p];const auto &config=state.pc_input[p];
      const bool aiming=(state.controller_ports.samples[p].buttons&0x10)!=0;
      // RGO: the mouse and the aim keys turn the aim (and the modern camera)
      // by angle deltas, never as a fake stick: nothing springs back when
      // input stops. Without the modern camera they only work while aiming.
      const auto key_down=[&](int key){return pc_key_down(state,key);};
      const bool mouse=state.pc_keyboard_active[p] && config.mouse_aim && state.pc_mouse_captured;
      const bool keys=state.pc_keyboard_active[p] && (config.modern || aiming) && jfg::PcKeyAim::held(config,key_down);
      jfg::PcLookDelta motion{};
      if(mouse || keys) {
        motion=state.pc_key_aim.take(config,key_down,key_aim_seconds);
        if(mouse)motion=jfg::pc_add_look(state.pc_mouse.take_angles(config,aiming),motion);
      }
      if(state.pc_pad_active[p] && config.dual_stick) {
        frame.pc_mode=config.modern?4:2;frame.look_x=state.pc_aim[p].x;frame.look_y=state.pc_aim[p].y;
        // RGO dual input: mouse/key angles join the stick as direct deltas.
        if(mouse || keys)state.pc_direct[p]=jfg::pc_add_look(state.pc_direct[p],motion);
      } else if(mouse || keys) {
        frame.pc_mode=config.modern?3:5;
        frame.look_x=static_cast<std::int16_t>(motion.x);frame.look_y=static_cast<std::int16_t>(motion.y);
      }
    }
  }
  if(block_physical && !state.input_replay_loaded) {
    buttons=0;stick_x=stick_y=0;
    for(auto& sample:state.controller_ports.samples){sample.buttons=0;sample.x=sample.y=0;}
  }
  // Map-driven automation may run while its tool window has focus. Settings
  // still suppress synthesized inputs below, as well as physical controller input.
  if (state.navigation_mod.enabled) {
    const bool escape = host_key_down(state, VK_ESCAPE) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool manual = buttons != 0U || stick_x != 0 || stick_y != 0 || escape;
    // Retain manual cancellation across room resets and between map exports.
    if (manual) ++state.navigation_mod.manual_inputs;
    if (escape) state.navigation_mod.pilot.stop("manual_takeover");
    const auto ai = state.navigation_mod.pilot.sample(manual, state.input_replay_loaded, true);
    if (!manual && !state.input_replay_loaded && state.navigation_mod.pilot.active()) {
      buttons = ai.buttons; stick_x = ai.x; stick_y = ai.y;
    }
    if (state.controller_samples % 6U == 0U) {
      jfg::mod::Memory memory({state.rdram,kRdramSize});
      const auto dialogue=navigation_dialogue(state);
      if(dialogue.known&&dialogue.active) {
        state.navigation_mod.pilot.stop("dialogue_active");
        if(!manual&&!state.input_replay_loaded){buttons=0;stick_x=stick_y=0;}
      }
      std::ifstream request(state.navigation_output/L"ai-dialogue.txt",std::ios::binary|std::ios::ate);
      if(request&&request.tellg()<=256) {
        request.seekg(0);
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        const int input=state.navigation_mod.dialogue_input.command(request,dialogue,manual,
          state.input_replay_loaded,memory.u32(0x800FB114U),state.navigation_mod.generation,
          state.navigation_mod.manual_inputs,now);
        if(input!=0){buttons=input==1?kButtonA:0;stick_x=0;stick_y=input==2?60:input==3?-60:0;}
      }
    }
    if (state.controller_samples % 6U == 0U) {
      const auto path = state.navigation_output / L"ai-confirm.txt";
      std::ifstream command(path, std::ios::binary | std::ios::ate);
      if (command && command.tellg() <= 256) {
        command.seekg(0);
        jfg::mod::Memory memory({state.rdram, kRdramSize});
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (state.navigation_mod.confirmation.command(command,
              jfg::mod::transition_confirmation(memory), manual, state.input_replay_loaded,
              memory.u32(0x800FB114U), state.navigation_mod.generation,
              state.navigation_mod.manual_inputs, now)) {
          buttons = kButtonA; stick_x = stick_y = 0;
        }
      }
    }
  }
  if(settings_capture && !state.input_replay_loaded) {
    buttons=0;stick_x=stick_y=0;
  }
  state.controller_ports.samples[0]={buttons,static_cast<std::int8_t>(stick_x),static_cast<std::int8_t>(stick_y),connected};
  if(state.input_replay_loaded) for(std::size_t p=1;p<4;++p)state.controller_ports.samples[p]={};
  if (connected != state.latched_controller_connected) {
    if (connected)
      ++state.controller_reconnects;
    else
      ++state.controller_disconnects;
  }
  state.latched_controller_connected = connected;
  state.latched_controller_buttons = buttons;
  state.latched_controller_stick_x = static_cast<std::int8_t>(stick_x);
  state.latched_controller_stick_y = static_cast<std::int8_t>(stick_y);
  trace_phase9_event(state, "input-poll", state.controller_samples);
  // Record the effective packet and separate look channel, including an
  // explicitly requested replay-to-recording conversion for qualification.
  if (state.input_record.is_open()) {
    state.input_record << state.vi_retraces << ',' << state.vi_retraces + 1U
                       << ',' << (connected ? 1 : 0) << ',' << std::hex
                       << buttons << std::dec << ',' << stick_x << ','
                       << stick_y;
    if(state.pc_record_extended) {
      const auto &pc=state.pc_frames[0];
      state.input_record << ',' << int(pc.pc_mode) << ',' << pc.look_x << ',' << pc.look_y;
    }
    state.input_record << '\n';
  }
  if (state.phase95_poll_trace.is_open()) {
    hle::GuestMemory memory({state.rdram, kRdramSize},
                            hle::GuestMemory::Layout::native_word_big_endian);
    std::uint32_t level = 0U;
    std::uint32_t rng = 0U;
    (void)memory.read_u32(0x800FB114U, level);
    (void)memory.read_u32(0x800A33E4U, rng);
    state.phase95_poll_trace
        << state.controller_samples << '\t' << state.vi_retraces << '\t'
        << state.vi_frames << '\t'
        << static_cast<unsigned>(state.rdram[0x000A51B0U ^ 3U]) << '\t'
        << level << '\t' << rng << '\t' << buttons << '\t'
        << stick_x << '\t' << stick_y << '\n';
  }
  ++state.controller_samples;
  state.gameplay_trace.event("input", {state.vi_retraces, state.controller_samples,
      connected ? 1U : 0U, buttons, static_cast<std::uint8_t>(stick_x),
      static_cast<std::uint8_t>(stick_y)});
  if (buttons != 0U || stick_x != 0 || stick_y != 0)
    ++state.non_neutral_controller_samples;
  if (!GetParent(static_cast<HWND>(state.native_window)) && !state.input_replay_loaded && !state.navigation_mod.enabled && host_key_down(state, VK_ESCAPE))
    state.exit_requested = true;
  state.host_key_presses.fill(false);
}

bool service_host_audio(State &state, const bool pace) {
  if (state.mmio_trace == nullptr || state.rdram == nullptr ||
      state.mmio_trace->ai_submission_overflows != 0U)
    return false;
  while (state.mmio_trace->ai_submission_read <
         state.mmio_trace->ai_submission_write) {
    const std::size_t slot = static_cast<std::size_t>(
        state.mmio_trace->ai_submission_read %
        MmioTrace::kAiSubmissionCapacity);
    const MmioTrace::AiSubmission submission =
        state.mmio_trace->ai_submissions[slot];
    if (!state.host_audio.queue(state.rdram, submission.address,
                                submission.length,
                                submission.dac_rate))
      return false;
    ++state.mmio_trace->ai_submission_read;
  }
  // Interactive mode is already paced by the VI/window clock.  Waiting on
  // the SDL queue as well creates a second, drifting clock and visible frame
  // hitches whenever a graphics frame arrives late.  Headless probes have no
  // window clock, so retain audio throttling there unless a diagnostic replay
  // explicitly uses the same VI clock as interactive playback.
  state.host_audio.service(
      pace, pace && !state.play_mode && !state.fast_replay &&
                !state.realtime_replay &&
                !(state.original_os_host_pacing && state.original_os_host_anchor_ready));
  state.audio_device_initialized = state.host_audio.initialized();
  state.audio_device_started = state.host_audio.started();
  state.audio_device_frequency = state.host_audio.frequency();
  state.audio_device_buffers = state.host_audio.submitted_buffers();
  state.audio_device_queued_bytes = state.host_audio.queued_bytes();
  state.audio_device_consumed_bytes = state.host_audio.consumed_bytes();
  state.audio_device_consumed_ms =
      state.host_audio.consumed_milliseconds();
  state.audio_device_underruns = state.host_audio.underruns();
  state.audio_device_overruns = state.host_audio.overruns();
  state.mmio_trace->ai_emulated_remaining_length =
      state.host_audio.current_buffer_remaining();
  return true;
}

#if defined(_WIN32)
// RGO: a high-resolution waitable timer (Windows 10 1803 and later) wakes
// within about half a millisecond whatever the process timer resolution is.
struct HighResolutionTimer {
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
      0x00000002U /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  HighResolutionTimer() = default;
  HighResolutionTimer(const HighResolutionTimer &) = delete;
  HighResolutionTimer &operator=(const HighResolutionTimer &) = delete;
  ~HighResolutionTimer() {
    if (handle != nullptr)
      CloseHandle(handle);
  }
};
#endif

// RGO frame pacing: sleep until a VI is due. Counts the time slept (the busy
// time excludes it) and how late the thread woke (the Detailed counter's
// "VI wake" figure: a plain sleep can wake up to 15.6 ms late on Windows when
// the timer resolution is not raised, which makes frames miss their VI).
void paced_sleep_until(State &state, const std::chrono::steady_clock::time_point deadline) {
  const auto start = std::chrono::steady_clock::now();
#if defined(_WIN32)
  // Each guest thread is a host thread, so each gets its own timer.
  thread_local HighResolutionTimer timer;
  jfg::precise_sleep_until(deadline, [](const std::chrono::nanoseconds wait) {
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(wait.count() / 100);  // relative, 100 ns units
    const auto timeout = static_cast<DWORD>(wait.count() / 1000000 + 20);
    if (timer.handle == nullptr || due.QuadPart >= 0 ||
        !SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) ||
        WaitForSingleObject(timer.handle, timeout) != WAIT_OBJECT_0)
      std::this_thread::sleep_for(wait);
  });
#else
  jfg::precise_sleep_until(deadline);
#endif
  const auto end = std::chrono::steady_clock::now();
  state.pacing_wait_us += static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
  if (end > deadline)
    state.vi_wake_late_max_us = (std::max)(state.vi_wake_late_max_us,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - deadline).count()));
}

// RGO display settings: follow the launcher's video settings while playing.
void refresh_video_settings(State &state) {
  if (!state.video_settings_checked) {
    state.video_settings_checked = true;
    wchar_t *path = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&path, &length, L"JFG_VIDEO_CONFIG") == 0 && path) {
      const std::filesystem::path config(path);
      std::free(path);
      // A missing or unreadable file means the default settings.
      state.video = jfg::VideoSettings{};
      state.video_managed = true;
      if (!config.empty()) {
        state.video_settings_file = std::make_unique<jfg::AsyncVolumeFile>(
            jfg::kVideoSettingsMaximumBytes);
        state.video_settings_file->start(config);
      }
    } else if (state.play_mode && !state.input_replay_loaded) {
      // Direct interactive runs without a launcher profile get the defaults.
      state.video = jfg::VideoSettings{};
      state.video_managed = true;
    } else {
      // Replays and captures keep the original picture and renderer setup.
      state.video = jfg::VideoSettings{jfg::AspectMode::original};
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (state.video_settings_file && now >= state.video_settings_next_read) {
    state.video_settings_next_read = now + std::chrono::milliseconds(250);
    // A partial replacement fails to parse and keeps the last setting.
    if (const auto text = state.video_settings_file->latest())
      if (const auto settings = jfg::parse_video_settings(*text))
        state.video = *settings;
  }
  state.widescreen = state.video.aspect == jfg::AspectMode::widescreen;
  // RGO fast clock: unless Original N64 slowdown is on (Original frame rate
  // only), CPU work costs almost no emulated time and waits skip to the next
  // device event (docs/rgo/framerate.md). Replays and captures keep the
  // original cost model so recorded inputs stay deterministic.
  state.fast_clock = state.video_managed && !state.input_replay_loaded &&
      jfg::fast_clock(state.video) ? 1U : 0U;
  if (state.rt64_shell != nullptr) {
    if (state.video_managed)
      state.rt64_shell->set_video_settings(state.video);
    state.widescreen_extent = state.rt64_shell->horizontal_extent();
    // RGO frame interpolation: this game frame took this many VIs (2 for
    // 30 fps, 3 for 20 fps); RT64 blends extra frames at that logic rate.
    if (state.last_frame_retrace != 0U)
      state.rt64_shell->set_game_rate(jfg::game_rate_from_retraces(
          state.vi_retraces - state.last_frame_retrace));
    state.last_frame_retrace = state.vi_retraces;
  }
  // RGO FPS counter (frame_stats.hpp), reported to the frontend twice a
  // second: game logic updates, real frames rendered and frames displayed
  // per second, and the displayed frame time range.
  ++state.frame_rate_presents;
  const std::uint64_t displayed = state.rt64_shell != nullptr
      ? state.rt64_shell->display_presents() : 0U;
  if (state.frame_rate_start == std::chrono::steady_clock::time_point{}) {
    state.frame_rate_start = now;
    state.frame_rate_presents = 0U;
    state.frame_rate_logic_start = state.logic_frames;
    state.frame_rate_display_start = displayed;
    if (state.rt64_shell != nullptr)
      (void)state.rt64_shell->take_display_intervals();
    state.frame_logic_ticks = 0U;
    state.frame_drawn = state.frame_new = state.frame_camera = state.frame_repeats = 0U;
    state.frame_gaps = {};
    state.frame_input_start = state.controller_samples;
    state.frame_busy_us = state.frame_busy_max_us = state.frame_busy_frames = 0U;
    state.vi_wake_late_max_us = 0U;
    if (state.mmio_trace != nullptr) {
      state.frame_mmio_start = state.mmio_trace->accesses;
      state.frame_mmio_stepped_start =
          state.mmio_trace->accesses - state.mmio_trace->emulated_accesses;
    }
    state.frame_switch_start = jfg::boot::baton_handoffs();
    state.frame_wake_start = jfg::boot::baton_sleeps();
    state.frame_graphics_start = state.graphics_tasks;
    state.frame_submit_us_start = state.graphics_submit_microseconds;
  } else if (now - state.frame_rate_start >= std::chrono::milliseconds(500)) {
    const auto elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - state.frame_rate_start).count());
    const auto per_second_tenths = [elapsed_us](const std::uint64_t count) {
      return jfg::frame_rate_tenths(count, elapsed_us);
    };
    const auto [display_min_us, display_max_us] = state.rt64_shell != nullptr
        ? state.rt64_shell->take_display_intervals()
        : std::pair<std::uint64_t, std::uint64_t>{};
    const jfg::FrameCounter counter{
        per_second_tenths(state.logic_frames - state.frame_rate_logic_start),
        per_second_tenths(state.frame_rate_presents),
        per_second_tenths(displayed - state.frame_rate_display_start),
        jfg::frame_time_tenth_ms(display_min_us), jfg::frame_time_tenth_ms(display_max_us)};
    // FPS counter "Detailed": what the game did with those frames.
    jfg::FrameStats details{};
    details.logic_tenths = counter.game_tenths;
    details.ticks_tenths = per_second_tenths(state.frame_logic_ticks);
    details.drawn_tenths = per_second_tenths(state.frame_drawn);
    details.new_tenths = per_second_tenths(state.frame_new);
    details.camera_tenths = per_second_tenths(state.frame_camera);
    details.repeat_tenths = per_second_tenths(state.frame_repeats);
    details.input_tenths = per_second_tenths(state.controller_samples - state.frame_input_start);
    details.frame_min = state.frame_gaps.minimum_tenths();
    details.frame_max = state.frame_gaps.maximum_tenths();
    details.display_min = counter.frame_min_tenth_ms;
    details.display_max = counter.frame_max_tenth_ms;
    // Host time per game frame, graphics tasks and the True 60 governor.
    const auto tenths_ms = [](const std::uint64_t microseconds) {
      return static_cast<std::uint32_t>((std::min<std::uint64_t>)((microseconds + 50U) / 100U, 0xFFFFU));
    };
    const std::uint64_t logic = state.logic_frames - state.frame_rate_logic_start;
    if (state.frame_busy_frames != 0U) {
      details.busy_avg = tenths_ms(state.frame_busy_us / state.frame_busy_frames);
      details.busy_max = tenths_ms(state.frame_busy_max_us);
    }
    if (logic != 0U) {
      details.tasks_per_frame = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
          ((state.graphics_tasks - state.frame_graphics_start) * 10U + logic / 2U) / logic, 0xFFFFU));
      details.submit_per_frame = tenths_ms(
          (state.graphics_submit_microseconds - state.frame_submit_us_start) / logic);
    }
    if (state.video_managed && !state.input_replay_loaded && jfg::true_logic_60(state.video)) {
      details.true60 = state.true60_governor.holding_30() ? 2U : 1U;
      details.late = state.true60_governor.late_frames();
      details.busy_p90 = tenths_ms(state.true60_governor.busy_p90());
    }
    details.wake_late = tenths_ms(state.vi_wake_late_max_us);
    // Runtime costs: device register traps (one-trap emulated or the slow
    // single step), guest thread switches and their wake-ups, audio cushion.
    const auto per_second = [elapsed_us](const std::uint64_t count) {
      return static_cast<std::uint32_t>((std::min<std::uint64_t>)(
          (count * 1000000U + elapsed_us / 2U) / elapsed_us, UINT32_MAX));
    };
    if (state.mmio_trace != nullptr) {
      const std::uint64_t accesses = state.mmio_trace->accesses;
      const std::uint64_t stepped = accesses - state.mmio_trace->emulated_accesses;
      details.mmio = per_second(accesses - state.frame_mmio_start);
      details.mmio_stepped = (std::min)(per_second(stepped - state.frame_mmio_stepped_start), 0xFFFFU);
    }
    details.switches = per_second(jfg::boot::baton_handoffs() - state.frame_switch_start);
    details.wakes = per_second(jfg::boot::baton_sleeps() - state.frame_wake_start);
    details.audio_cushion = state.host_audio.cushion_milliseconds();
    details.audio_queued = state.host_audio.smoothed_queue_milliseconds();
    details.audio_underruns = static_cast<std::uint32_t>(state.host_audio.underruns());
    const HWND window = static_cast<HWND>(state.native_window);
    if (window != nullptr && GetParent(window) != nullptr) {
      const HWND root = GetAncestor(window, GA_ROOT);
      const auto packed = jfg::pack_frame_counter(counter);
      PostMessageW(root, WM_APP + jfg::kFrameStatsMessageOffset,
                   static_cast<WPARAM>(packed.first), static_cast<LPARAM>(packed.second));
      const auto words = jfg::encode_frame_stats(details);
      for (unsigned index = 0U; index < jfg::kFrameDetailMessageCount; ++index)
        PostMessageW(root, WM_APP + jfg::kFrameDetailMessageOffset + index,
                     static_cast<WPARAM>(words[index * 2U]),
                     static_cast<LPARAM>(words[index * 2U + 1U]));
    }
#if defined(JFG_HEADLESS_LIVE)
    if (state.counter_log) {
      std::string host = jfg::format_frame_host(details);
      std::replace(host.begin(), host.end(), '\n', ' ');
      std::fprintf(stderr, "counter: VI %llu %s | %s | audio queued %u ms, underruns %llu, trim %+.1f ms | squaddie calls %llu | snapshot imports delta %llu full %llu, chunks %llu, verify mismatches %llu\n",
                   static_cast<unsigned long long>(state.vi_retraces),
                   jfg::format_frame_counter(counter).c_str(), host.c_str(),
                   state.host_audio.queued_milliseconds(),
                   static_cast<unsigned long long>(state.host_audio.underruns()),
                   static_cast<double>(state.audio_trim_ns) / 1e6,
                   static_cast<unsigned long long>(state.squaddie_calls),
                   static_cast<unsigned long long>(state.snapshot_delta_imports),
                   static_cast<unsigned long long>(state.snapshot_full_imports),
                   static_cast<unsigned long long>(state.snapshot_delta_chunks),
                   static_cast<unsigned long long>(state.snapshot_verify_mismatches));
    }
#endif
    state.frame_rate_start = now;
    state.frame_rate_presents = 0U;
    state.frame_rate_logic_start = state.logic_frames;
    state.frame_rate_display_start = displayed;
    state.frame_logic_ticks = 0U;
    state.frame_drawn = state.frame_new = state.frame_camera = state.frame_repeats = 0U;
    state.frame_gaps = {};
    state.frame_input_start = state.controller_samples;
    state.frame_busy_us = state.frame_busy_max_us = state.frame_busy_frames = 0U;
    state.vi_wake_late_max_us = 0U;
    if (state.mmio_trace != nullptr) {
      state.frame_mmio_start = state.mmio_trace->accesses;
      state.frame_mmio_stepped_start =
          state.mmio_trace->accesses - state.mmio_trace->emulated_accesses;
    }
    state.frame_switch_start = jfg::boot::baton_handoffs();
    state.frame_wake_start = jfg::boot::baton_sleeps();
    state.frame_graphics_start = state.graphics_tasks;
    state.frame_submit_us_start = state.graphics_submit_microseconds;
  }
}

void service_live_window(State &state) {
  state.window_input.consume(state.host_keys,state.host_key_presses,state.exit_requested);
  {
    const auto input=state.window_input.take_motion();
    if(input.focus_lost){state.pc_mouse.clear();state.pc_wheel.clear();state.pc_input_blocked=true;}
    if(state.pc_mouse_captured.load())state.pc_mouse.add(input.x,input.y);
    state.pc_wheel.add(input.wheel);
  }
  const SlowHostWork window_service_timing(state,"host-window-service");
  const auto frame_start = std::chrono::steady_clock::now();
  if (state.host_frame_start_initialized) {
    const auto interval = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            frame_start - state.host_frame_start)
            .count());
    state.host_frame_interval_max_us =
        (std::max)(state.host_frame_interval_max_us, interval);
    state.host_frames_over_25ms += interval > 25000U ? 1U : 0U;
    state.host_frames_over_50ms += interval > 50000U ? 1U : 0U;
    state.host_frames_over_100ms += interval > 100000U ? 1U : 0U;
    if (state.timing_trace) {
      state.timing_trace << "frame\t" << state.vi_frames << '\t'
                         << state.vi_retraces << '\t' << interval << '\t'
                         << state.presented_frames << '\t'
                         << state.pending_graphics_tasks.size() << '\n';
    }
  }
  state.host_frame_start = frame_start;
  state.host_frame_start_initialized = true;
  auto presentation = jfg::vi_presentation_size(
      state.vi_mode_horizontal_start, state.rt64_vi.vertical_start);
  // RGO widescreen: a standalone window opens out to at least 16:9; the
  // renderer then fills whatever shape the player gives it.
  if (state.widescreen && presentation.valid() &&
      presentation.width * 9U < presentation.height * 16U)
    presentation = {presentation.height * 16U / 9U, presentation.height};
  if (state.native_window != nullptr && presentation.valid() &&
      presentation.width * state.window_presentation.height !=
          state.window_presentation.width * presentation.height) {
    state.window_presentation = presentation;
    PostMessageW(static_cast<HWND>(state.native_window),LiveRt64Window::kAspectMessage,
        presentation.width,static_cast<LPARAM>(presentation.height));
  }
  const auto frontend = state.native_window ? GetAncestor(static_cast<HWND>(state.native_window), GA_ROOT) : nullptr;
  bool frontend_paused = false;
  while (!state.exit_requested && frontend && IsWindow(frontend) &&
         GetPropW(frontend, L"JfgFrontendPause") != nullptr) {
    if (!frontend_paused) {
      stop_rumble(state);
      state.host_audio.frontend_pause(true);
      SetPropW(frontend,L"JfgFrontendPaused",reinterpret_cast<HANDLE>(1));
      frontend_paused = true;
    }
    state.window_input.consume(state.host_keys,state.host_key_presses,state.exit_requested);
  {
    const auto input=state.window_input.take_motion();
    if(input.focus_lost){state.pc_mouse.clear();state.pc_wheel.clear();state.pc_input_blocked=true;}
    if(state.pc_mouse_captured.load())state.pc_mouse.add(input.x,input.y);
    state.pc_wheel.add(input.wheel);
  }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (frontend_paused) {
    state.busy_frame_start = {};  // the paused time is not host work
    RemovePropW(frontend,L"JfgFrontendPaused");
    state.host_audio.frontend_pause(false);
    state.host_frame_deadline_initialized = false;
    state.host_frame_start_initialized = false;
    state.original_os_host_anchor_ready = false;
  }
  if (state.original_os_host_pacing) return;
  if (!state.host_frame_deadline_initialized) {
    state.host_frame_deadline = std::chrono::steady_clock::now();
    state.host_frame_deadline_initialized = true;
  }
  state.host_frame_deadline += std::chrono::microseconds(16667);
  const auto deadline_now = std::chrono::steady_clock::now();
  if (state.host_frame_deadline > deadline_now) {
    paced_sleep_until(state, state.host_frame_deadline);
  } else if (deadline_now - state.host_frame_deadline >
             std::chrono::microseconds(66668)) {
    // Do not run an unbounded catch-up burst after shader compilation, window
    // movement, or another long host stall.  Resume from the current VI edge.
    state.host_frame_deadline = deadline_now;
  }
}

bool write_live_frame_capture(State &state) {
  if (state.frame_capture_path.empty())
    return true;
  if (state.rt64_shell == nullptr)
    return false;
  const jfg::Rt64FrameView frame = state.rt64_shell->last_presented_frame();
  if (!frame.valid())
    return false;
  std::ofstream stream(state.frame_capture_path, std::ios::binary);
  if (!stream)
    return false;
  stream << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
  for (std::size_t y = 0U; y < frame.height; ++y) {
    const std::size_t row = y * frame.row_pitch_bytes;
    for (std::size_t x = 0U; x < frame.width; ++x) {
      const std::size_t pixel = row + x * 4U;
      const std::array<char, 3U> rgb = {
          static_cast<char>(
              std::to_integer<unsigned char>(frame.bgra8[pixel + 2U])),
          static_cast<char>(
              std::to_integer<unsigned char>(frame.bgra8[pixel + 1U])),
          static_cast<char>(
              std::to_integer<unsigned char>(frame.bgra8[pixel]))};
      stream.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
  }
  state.frame_captured = static_cast<bool>(stream);
  return state.frame_captured;
}

bool write_logical_rdram_capture(const State &state,
                                 const std::filesystem::path &path) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::array<char, 4096U> logical_bytes{};
  for (std::size_t base = 0U; base < kRdramSize;
       base += logical_bytes.size()) {
    for (std::size_t index = 0U; index < logical_bytes.size(); ++index) {
      logical_bytes[index] =
          static_cast<char>(state.rdram[(base + index) ^ 3U]);
    }
    stream.write(logical_bytes.data(),
                 static_cast<std::streamsize>(logical_bytes.size()));
  }
  return static_cast<bool>(stream);
}

bool write_private_rdram_capture(const State &state) {
  if (state.rdram_capture_path.empty())
    return true;
  return write_logical_rdram_capture(state, state.rdram_capture_path);
}

bool write_private_rt64_snapshot(
    const State &state, const std::span<const std::byte> snapshot) {
  if (state.rdram_capture_path.empty())
    return true;
  std::ofstream stream(state.rdram_capture_path + ".rt64",
                       std::ios::binary);
  if (!stream || snapshot.empty() || snapshot.size() % 4U != 0U)
    return false;
  std::array<char, 4096U> logical_bytes{};
  for (std::size_t base = 0U; base < snapshot.size();
       base += logical_bytes.size()) {
    const std::size_t chunk =
        (std::min)(logical_bytes.size(), snapshot.size() - base);
    for (std::size_t index = 0U; index < chunk; ++index) {
      logical_bytes[index] = static_cast<char>(
          std::to_integer<std::uint8_t>(snapshot[(base + index) ^ 3U]));
    }
    stream.write(logical_bytes.data(), static_cast<std::streamsize>(chunk));
  }
  return static_cast<bool>(stream);
}

#if defined(JFG_LIVE_RUNTIME)
bool write_mod_export(State &state, const std::filesystem::path &path, const std::string &text) {
  const auto copy = std::make_shared<const std::string>(text);
  if (path.filename() == L"mesh.json") {
    state.navigation_mesh_export = copy;
    return true;
  }
  jfg::AsyncSnapshots::Batch files;
  if (state.navigation_mesh_export)
    files.push_back({state.navigation_output/L"mesh.json",state.navigation_mesh_export});
  files.push_back({path,copy});
  return state.snapshot_writer.submit(1U,std::move(files));
}

void update_navigation_mod(State &state) {
  if(!state.navigation_mod.observing())return;
  auto &mod=state.navigation_mod;
  mod.dialogue=navigation_dialogue(state);
  jfg::mod::Memory memory({state.rdram,kRdramSize});
  try {
    const auto list=jfg::mod::actors(memory);
    if (mod.updates % 6U == 0U) {
      std::ifstream settings(state.navigation_output / L"mods.txt", std::ios::binary | std::ios::ate);
      if (settings && settings.tellg() <= 128) { settings.seekg(0); (void)mod.configure_mods(settings); }
      std::ifstream warp(state.navigation_output / L"warp-exit.txt", std::ios::binary | std::ios::ate);
      if (warp && warp.tellg() <= 256) {
        warp.seekg(0);
        const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        (void)mod.warp_to_exit(warp, memory, list, stamp);
      }
    }
    bool player_present=false;
    for(const auto &actor:list) {
      if(actor.address==mod.player)player_present=true;
      (void)mod.clear_enemy(memory,actor.address);
    }
    if(!player_present)mod.player=0;
    else (void)mod.full_health(memory,mod.player);
    ++mod.updates;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto ai_level = memory.u32(0x800FB114U);
    if (mod.enabled && mod.updates % 6U == 0U) {
      const auto command_path = state.navigation_output / L"ai-command.txt";
      std::ifstream command(command_path, std::ios::binary | std::ios::ate);
      if (command && command.tellg() <= 32768) { command.seekg(0); (void)mod.pilot.command(command, ai_level, mod.generation, now); }
    }
    if (mod.enabled && mod.player != 0U) {
      const auto p = jfg::mod::position(memory, mod.player);
      const auto camera = jfg::mod::control_camera(memory);
      mod.pilot.camera_heading(camera.known, camera.yaw);
      mod.pilot.tick({p.x,p.y,p.z}, jfg::mod::gameplay_active(memory,mod.player),ai_level,mod.generation,now);
      const auto movement=jfg::mod::player_motion(memory,mod.player);
      if(movement.known && (movement.state==6U||movement.state==12U) &&
         mod.pilot.active() && std::string_view(mod.pilot.state).starts_with("jump_"))
        mod.pilot.stop("jump_unexpected_grab_state");
      // Diagnostic rows are immutable copies; the bounded worker never reads
      // guest memory. Slow storage must not stall simulation or audio.
      if(jfg::mod::gameplay_active(memory,mod.player)) {
        std::ostringstream trace;
        trace<<mod.updates<<','<<now<<','<<ai_level<<','<<p.x<<','<<p.y<<','<<p.z
             <<','<<movement.known<<','<<movement.state<<','<<movement.animation
             <<','<<movement.animation_frame<<','<<memory.u32(0x800F6DA0U)<<'\n';
        (void)state.movement_trace.append(trace.str());
      }
      if(std::string_view(mod.pilot.state).starts_with("jump_") && mod.pilot.active()) {
        std::ostringstream trace;const auto input=mod.pilot.observed_input();
        trace<<mod.updates<<','<<now<<','<<ai_level<<','<<p.x<<','<<p.y<<','<<p.z
             <<','<<input.x<<','<<input.y<<','<<input.buttons<<','<<mod.pilot.state
             <<','<<camera.yaw<<','<<memory.s16(mod.player)<<'\n';
        (void)state.jump_trace.append(trace.str());
      }
    } else mod.pilot.stop("player_unavailable");
    if(mod.updates%6U!=0U)return;
    const auto track=memory.u32(0x800A0D60U), level=memory.u32(0x800FB114U);
    bool mesh_ready=mod.player!=0U && track==mod.last_track && level==mod.last_level;
    if(mod.player!=0U && !mesh_ready) {
      try {
        const auto mesh=jfg::mod::decode_mesh(memory,track);
        std::ostringstream output;jfg::mod::write_mesh(output,mesh,level,mod.generation);
        mesh_ready=write_mod_export(state, state.navigation_output/L"mesh.json",output.str());
        if(mesh_ready){mod.last_track=track;mod.last_level=level;}
      }catch(const std::runtime_error &){++mod.invalid;}
    }
    std::ostringstream output;mod.write_state(output,memory,list,level,mesh_ready);
    if(!write_mod_export(state, state.navigation_output/L"live.json",output.str()))++mod.invalid;
  }catch(const std::runtime_error &){++mod.invalid;}
}
#endif

void trace_gameplay_state(State &state, bool update) {
  if (!state.gameplay_trace || state.rdram == nullptr) return;
  if (!update && state.gameplay_trace_vi == state.vi_retraces) return;
  if (update) ++state.gameplay_trace_updates;
  else state.gameplay_trace_vi = state.vi_retraces;
  hle::GuestMemory memory({state.rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t level = 0, actors = 0, count = 0;
  (void)memory.read_u32(0x800FB114U, level);
  (void)memory.read_u32(0x800F2CA4U, actors);
  (void)memory.read_u32(0x800F2CA8U, count);
  state.gameplay_trace.event(update ? "update" : "tick", {
      state.vi_retraces, state.controller_samples, state.gameplay_trace_updates,
      state.rdram[0xA51B0U ^ 3U], level, count, state.presented_frames,
      state.graphics_tasks, state.pending_graphics_tasks.size(), state.dispatch_calls,
      state.audio_device_queued_bytes, state.audio_device_consumed_bytes,
      state.audio_device_underruns, state.audio_device_overruns});
  if (!update) return;
  if (count > 256U) {
    state.gameplay_trace.event("actor-table-invalid", {state.vi_retraces, actors, count});
    return;
  }
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t actor = 0, header = 0;
    if (index >= count || !memory.read_u32(actors + index * 4U, actor) || actor == 0U) {
      if (state.gameplay_actor_seen[index])
        state.gameplay_trace.event("actor-removed", {state.vi_retraces, state.gameplay_trace_updates, index});
      state.gameplay_actor_seen[index] = false;
      continue;
    }
    // All actor types, sampled after the game update, only when state changes.
    // Payload: identity/name, actor[0..0x7f], properties[0..0x1f], control[0..0x1f].
    std::array<std::uint64_t, 56> row{};
    row[0] = state.vi_retraces; row[1] = state.gameplay_trace_updates;
    row[2] = index; row[3] = actor;
    (void)memory.read_u32(actor + 0x40U, header);
    for (std::uint32_t n = 0; n < 4U; ++n) {
      std::uint32_t word = 0;
      row[4U + n] = memory.read_u32(header + 4U + n * 4U, word) ? word : UINT32_MAX;
    }
    for (std::uint32_t n = 0; n < 32U; ++n) {
      std::uint32_t word = 0;
      row[8U + n] = memory.read_u32(actor + n * 4U, word) ? word : UINT32_MAX;
    }
    for (std::uint32_t block = 0; block < 2U; ++block) {
      std::uint32_t pointer = 0;
      (void)memory.read_u32(actor + (block == 0U ? 0x4CU : 0x68U), pointer);
      for (std::uint32_t n = 0; n < 8U; ++n) {
        std::uint32_t word = 0;
        row[40U + block * 8U + n] = pointer != 0U && memory.read_u32(pointer + n * 4U, word) ? word : UINT32_MAX;
      }
    }
    const auto &previous = state.gameplay_actor_previous[index];
    if (!state.gameplay_actor_seen[index] ||
        !std::equal(row.begin() + 2, row.end(), previous.begin() + 2)) {
      state.gameplay_trace.event("actor", row);
      state.gameplay_actor_previous[index] = row;
      state.gameplay_actor_seen[index] = true;
    }
  }
}

void write_actor_timing_trace(State &state) {
  if (!state.actor_timing_trace || !state.timing_trace.is_open() ||
      state.rdram == nullptr ||
      state.last_actor_trace_retrace == state.vi_retraces)
    return;
  state.last_actor_trace_retrace = state.vi_retraces;
  hle::GuestMemory actor_memory(
      {state.rdram, kRdramSize},
      hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t actor_list = 0U;
  std::uint32_t actor_count = 0U;
  if (!actor_memory.read_u32(0x800F2CA4U, actor_list) ||
      !actor_memory.read_u32(0x800F2CA8U, actor_count) || actor_count > 256U)
    return;
  for (std::uint32_t index = 0U; index < actor_count; ++index) {
    std::uint32_t actor = 0U;
    std::uint32_t header = 0U;
    std::uint32_t name0 = 0U;
    std::uint32_t name1 = 0U;
    if (!actor_memory.read_u32(actor_list + index * 4U, actor) ||
        actor == 0U || !actor_memory.read_u32(actor + 0x40U, header) ||
        header == 0U || !actor_memory.read_u32(header + 4U, name0) ||
        !actor_memory.read_u32(header + 8U, name1) ||
        name0 != 0x616E696DU || name1 != 0x426C7565U)
      continue;
    state.timing_trace << "actor\t" << state.vi_frames << '\t'
                       << state.vi_retraces << "\t0\t" << std::hex
                       << actor << '\t' << name0 << name1 << "\t0\t";
    for (std::uint32_t offset = 0U; offset < 0x200U; offset += 4U) {
      std::uint32_t word = 0U;
      if (offset != 0U)
        state.timing_trace << ':';
      if (actor_memory.read_u32(actor + offset, word))
        state.timing_trace << word;
      else
        state.timing_trace << "invalid";
    }
    state.timing_trace << std::dec << '\n';
  }
}

std::optional<std::string> hash_logical_rdram_region(
    const State &state, const std::uint32_t guest_address,
    const std::size_t length) {
  if (state.rdram == nullptr)
    return std::nullopt;
  const std::uint64_t physical = guest_address & 0x1FFFFFFFU;
  if (physical > kRdramSize || length > kRdramSize - physical)
    return std::nullopt;
  jfg::testkernel::Sha256 hasher;
  std::array<std::uint8_t, 1024U> logical{};
  std::size_t consumed = 0U;
  while (consumed < length) {
    const std::size_t chunk =
        (std::min)(logical.size(), length - consumed);
    for (std::size_t index = 0U; index < chunk; ++index)
      logical[index] = state.rdram[
          (static_cast<std::size_t>(physical) + consumed + index) ^ 3U];
    hasher.update_bytes(logical.data(), chunk);
    consumed += chunk;
  }
  return jfg::testkernel::hex_digest(hasher.finish());
}

void write_retrace_semantic_hash(State &state, std::ostream *update_output = nullptr,
                                std::ostream *poll_output = nullptr) {
  if ((update_output != nullptr && poll_output != nullptr) ||
      (update_output == nullptr && poll_output == nullptr &&
       !state.retrace_hash_trace.is_open()) ||
      state.rdram == nullptr)
    return;
  hle::GuestMemory memory({state.rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  constexpr std::uint32_t kObjectListAddress = 0x800F2CA4U;
  constexpr std::uint32_t kObjectCountAddress = 0x800F2CA8U;
  constexpr std::uint32_t kRngSeedAddress = 0x800A33E4U;
  constexpr std::uint32_t kFrontModeAddress = 0x800A51B0U;
  constexpr std::uint32_t kGameGlobalsAddress = 0x800A4FCCU;
  constexpr std::size_t kGameGlobalsBytes = 0x90U;
  constexpr std::uint32_t kCameraStateAddress = 0x801045B0U;
  constexpr std::size_t kCameraStateBytes = 0x158U;
  constexpr std::size_t kActorStateBytes = 0x200U;
  constexpr std::uint32_t kMaximumActors = 256U;

  std::uint32_t actor_list = 0U;
  std::uint32_t actor_count = 0U;
  std::uint32_t rng_seed = 0U;
  (void)memory.read_u32(kObjectListAddress, actor_list);
  (void)memory.read_u32(kObjectCountAddress, actor_count);
  (void)memory.read_u32(kRngSeedAddress, rng_seed);
  const unsigned front_mode = state.rdram[(kFrontModeAddress - kKseg0) ^ 3U];
  const bool actor_table_valid = actor_count <= kMaximumActors &&
      (actor_count == 0U ||
       hash_logical_rdram_region(state, actor_list,
                                 actor_count * sizeof(std::uint32_t))
           .has_value());
  const auto actor_table_hash = actor_table_valid
      ? hash_logical_rdram_region(state, actor_list,
                                  actor_count * sizeof(std::uint32_t))
      : std::nullopt;
  const auto player_hash = state.phase9_player_actor == 0U
      ? std::nullopt
      : hash_logical_rdram_region(state, state.phase9_player_actor,
                                  kActorStateBytes);
  const auto globals_hash = hash_logical_rdram_region(
      state, kGameGlobalsAddress, kGameGlobalsBytes);
  const auto camera_hash = hash_logical_rdram_region(
      state, kCameraStateAddress, kCameraStateBytes);

  auto &stream = poll_output != nullptr ? *poll_output
      : update_output != nullptr ? *update_output : state.retrace_hash_trace;
  stream << "{\"kind\":\""
         << (poll_output != nullptr ? "jfg-phase9-poll-hash"
             : update_output == nullptr ? "jfg-phase9-retrace-hash"
                                        : "jfg-phase9-update-hash")
         << "\",\"schema\":1,\""
         << (poll_output != nullptr ? "poll"
             : update_output == nullptr ? "retrace" : "update") << "\":"
         << (poll_output != nullptr ? state.controller_samples - 1U
             : update_output == nullptr ? state.vi_retraces
                                        : state.completed_game_updates)
         << ",\"front_mode\":" << front_mode
         << ",\"rng_seed\":\"0x" << std::hex << std::setw(8)
         << std::setfill('0') << rng_seed
         << "\",\"player_actor\":\"0x" << std::setw(8)
         << state.phase9_player_actor << "\"" << std::dec
         << ",\"player_sha256\":";
  if (player_hash.has_value())
    stream << '"' << *player_hash << '"';
  else
    stream << "null";
  stream << ",\"actor_list\":\"0x" << std::hex << std::setw(8)
         << std::setfill('0') << actor_list << "\"" << std::dec
         << ",\"actor_count\":" << actor_count
         << ",\"actor_table_sha256\":";
  if (actor_table_hash.has_value())
    stream << '"' << *actor_table_hash << '"';
  else
    stream << "null";
  stream << ",\"globals_sha256\":";
  if (globals_hash.has_value())
    stream << '"' << *globals_hash << '"';
  else
    stream << "null";
  stream << ",\"camera_sha256\":";
  if (camera_hash.has_value())
    stream << '"' << *camera_hash << '"';
  else
    stream << "null";
  stream << ",\"actors\":[";
  bool first = true;
  if (actor_table_valid) {
    for (std::uint32_t index = 0U; index < actor_count; ++index) {
      std::uint32_t actor = 0U;
      (void)memory.read_u32(actor_list + index * sizeof(std::uint32_t), actor);
      const auto digest = actor == 0U
          ? std::nullopt
          : hash_logical_rdram_region(state, actor, kActorStateBytes);
      if (!first)
        stream << ',';
      first = false;
      stream << "{\"index\":" << index << ",\"address\":\"0x"
             << std::hex << std::setw(8) << std::setfill('0') << actor
             << "\",\"sha256\":" << std::dec;
      if (digest.has_value())
        stream << '"' << *digest << '"';
      else
        stream << "null";
      stream << '}';
    }
  }
  stream << ']';
  if (poll_output != nullptr) {
    std::uint32_t level_word = 0U;
    (void)memory.read_u32(0x800FB114U, level_word);
    stream << ",\"level_word\":" << level_word
           << ",\"connected\":" << (state.latched_controller_connected ? 1 : 0)
           << ",\"buttons\":" << state.latched_controller_buttons
           << ",\"stick_x\":" << static_cast<int>(state.latched_controller_stick_x)
           << ",\"stick_y\":" << static_cast<int>(state.latched_controller_stick_y)
           << ",\"update_counter_valid\":"
           << (state.update_hash_trace.is_open() ? "true" : "false")
           << ",\"completed_updates\":";
    if (state.update_hash_trace.is_open())
      stream << state.completed_game_updates;
    else
      stream << "null";
    stream << ",\"vi_retraces\":" << state.vi_retraces
           << ",\"controller_read_start_calls\":"
           << state.controller_read_start_calls
           << ",\"controller_get_data_calls\":"
           << state.controller_get_data_calls;
  } else if (update_output != nullptr)
    stream << ",\"controller_polls\":" << state.controller_samples
           << ",\"vi_retraces\":" << state.vi_retraces;
  stream << "}\n";
}

void write_private_progress(State &state,
                            const char *stage = "retrace") {
  constexpr std::uint64_t kProgressIntervalRetraces = 30U;
  // Actor motion needs per-retrace sampling to expose short freeze/jump
  // patterns. Deduplication keeps the many progress call sites from writing
  // the same actor state more than once in a retrace.
  trace_gameplay_state(state, false);
  write_actor_timing_trace(state);
  // This diagnostic contains sizeable rolling renderer traces.  Rewriting it
  // at every begin/end marker can turn optional observability into several
  // synchronous filesystem writes per video frame and make an otherwise
  // smooth build hitch.  One atomic snapshot per half-second is sufficient
  // for watchdog diagnosis and keeps instrumentation off the critical path.
  if (state.progress_path.empty() ||
      state.vi_retraces % kProgressIntervalRetraces != 0U ||
      state.last_progress_retrace == state.vi_retraces)
    return;
  state.last_progress_retrace = state.vi_retraces;
  std::ostringstream stream;
  const unsigned front_mode = state.rdram[0x000A51B0U ^ 3U];
  const std::uint32_t last_dispatch =
      state.recent_dispatch_position == 0U
          ? 0U
          : state.recent_dispatches[(state.recent_dispatch_position - 1U) %
                                    state.recent_dispatches.size()];
  stream << "{\"stage\":\"" << stage << "\""
         << ",\"vi_retraces\":" << state.vi_retraces
         << ",\"front_mode\":" << front_mode
         << ",\"dispatches\":" << state.dispatch_calls
         << ",\"last_dispatch\":\"0x" << std::hex << last_dispatch
         << std::dec << '"'
         << ",\"generated_calls\":" << state.generated_calls
         << ",\"sync_print_sink_calls\":" << state.sync_print_sink_calls
         << ",\"sound_queue_recoveries\":" << state.sound_queue_recoveries
         << ",\"mapped_calls\":" << state.mapped_calls
         << ",\"interrupt_timeslices\":" << state.interrupt_timeslices
         << ",\"receive_successes\":" << state.receive_successes
         << ",\"receive_empties\":" << state.receive_empties
         << ",\"receive_blocks\":" << state.receive_blocks
         << ",\"last_receive_queue\":\"0x" << std::hex
         << state.last_receive_queue << "\""
         << ",\"last_receive_block\":" << std::dec
         << state.last_receive_block
         << ",\"last_receive_result\":" << state.last_receive_result
         << ",\"graphics_tasks\":" << state.graphics_tasks
         << ",\"presented_frames\":" << state.presented_frames
         << ",\"vi_current_framebuffer\":\"0x" << std::hex
         << state.vi_current_framebuffer
         << "\",\"vi_next_framebuffer\":\"0x"
         << state.vi_next_framebuffer
         << "\",\"graphics_color_image\":\"0x"
         << state.graphics_color_image
         << "\",\"recent_graphics_targets\":\"";
  const std::size_t graphics_recent_count =
      (std::min)(state.recent_graphics_position,
                 state.recent_graphics_color_images.size());
  for (std::size_t index = 0U; index < graphics_recent_count; ++index) {
    const std::size_t distance = graphics_recent_count - index;
    const std::size_t slot =
        (state.recent_graphics_position - distance) %
        state.recent_graphics_color_images.size();
    if (index != 0U)
      stream << ',';
    stream << state.recent_graphics_command_addresses[slot] << ':'
           << state.recent_graphics_color_images[slot] << ':'
           << state.recent_graphics_vi_selections[slot] << ':'
           << state.recent_graphics_matrix_commands[slot] << ':'
           << state.recent_graphics_vertices_loaded[slot] << ':'
           << state.recent_graphics_vertices_in_clip[slot] << ':'
           << state.recent_graphics_triangles[slot];
  }
  stream << "\"" << std::dec
         << ",\"decoded_audio_tasks\":" << state.decoded_audio_tasks
         << ",\"controller_samples\":" << state.controller_samples
         << ",\"non_neutral_controller_samples\":"
         << state.non_neutral_controller_samples
         << ",\"non_neutral_controller_writes\":"
         << state.non_neutral_controller_writes
         << ",\"game_pressed_observations\":"
         << state.game_pressed_observations
         << ",\"game_stick_observations\":"
         << state.game_stick_observations
         << ",\"last_game_stick_x\":"
         << static_cast<int>(state.last_game_stick_x)
         << ",\"last_game_stick_y\":"
         << static_cast<int>(state.last_game_stick_y)
         << ",\"menu_stick_observations\":"
         << state.menu_stick_observations
         << ",\"last_menu_stick_x\":"
         << static_cast<int>(state.last_menu_stick_x)
         << ",\"last_menu_stick_y\":"
         << static_cast<int>(state.last_menu_stick_y)
         << ",\"ai_register_writes\":"
         << (state.mmio_trace == nullptr
                 ? 0U
                 : state.mmio_trace->ai_register_writes)
         << ",\"ai_buffer_submissions\":"
         << (state.mmio_trace == nullptr
                 ? 0U
                 : state.mmio_trace->ai_buffer_submissions)
         << ",\"ai_dram_address\":\"0x" << std::hex
         << (state.mmio_trace == nullptr
                 ? 0U
                 : state.mmio_trace->ai_dram_address)
         << "\",\"ai_length\":" << std::dec
         << (state.mmio_trace == nullptr ? 0U
                                         : state.mmio_trace->ai_length)
         << ",\"ai_control\":"
         << (state.mmio_trace == nullptr ? 0U
                                         : state.mmio_trace->ai_control)
         << ",\"ai_dac_rate\":"
         << (state.mmio_trace == nullptr ? 0U
                                         : state.mmio_trace->ai_dac_rate)
         << ",\"ai_bit_rate\":"
         << (state.mmio_trace == nullptr ? 0U
                                         : state.mmio_trace->ai_bit_rate)
         << ",\"audio_task_us\":" << state.audio_task_microseconds
         << ",\"graphics_prepare_us\":"
         << state.graphics_prepare_microseconds
         << ",\"graphics_submit_us\":"
         << state.graphics_submit_microseconds
         << ",\"graphics_present_us\":"
         << state.graphics_present_microseconds
         << ",\"host_frame_interval_max_us\":"
         << state.host_frame_interval_max_us
         << ",\"present_interval_max_us\":"
         << state.present_interval_max_us
         << ",\"graphics_prepare_max_us\":"
         << state.graphics_prepare_max_us
         << ",\"graphics_submit_max_us\":"
         << state.graphics_submit_max_us
         << ",\"graphics_present_max_us\":"
         << state.graphics_present_max_us
         << ",\"host_frames_over_25ms\":"
         << state.host_frames_over_25ms
         << ",\"host_frames_over_50ms\":"
         << state.host_frames_over_50ms
         << ",\"host_frames_over_100ms\":"
         << state.host_frames_over_100ms
         << ",\"recent_matrix_conversions\":\"" << std::hex;
  const std::size_t matrix_recent_count =
      (std::min)(state.recent_matrix_conversion_position,
                 state.recent_matrix_conversion_sources.size());
  for (std::size_t index = 0U; index < matrix_recent_count; ++index) {
    const std::size_t distance = matrix_recent_count - index;
    const std::size_t slot =
        (state.recent_matrix_conversion_position - distance) %
        state.recent_matrix_conversion_sources.size();
    if (index != 0U)
      stream << ',';
    stream << state.recent_matrix_conversion_sources[slot] << ':'
           << state.recent_matrix_conversion_destinations[slot];
    for (const std::uint32_t word :
         state.recent_matrix_conversion_first_rows[slot])
      stream << ':' << word;
    for (const std::uint32_t word :
         state.recent_matrix_conversion_output_words[slot])
      stream << ':' << word;
  }
  stream << '"' << std::dec
         << ",\"recent_matrix_cats\":\"" << std::hex;
  const std::size_t matrix_cat_recent_count =
      (std::min)(state.recent_matrix_cat_position,
                 state.recent_matrix_cat_args.size());
  for (std::size_t index = 0U; index < matrix_cat_recent_count; ++index) {
    const std::size_t distance = matrix_cat_recent_count - index;
    const std::size_t slot =
        (state.recent_matrix_cat_position - distance) %
        state.recent_matrix_cat_args.size();
    if (index != 0U)
      stream << ',';
    for (const std::uint32_t argument : state.recent_matrix_cat_args[slot])
      stream << argument << ':';
    for (std::size_t word = 0U;
         word < state.recent_matrix_cat_words[slot].size(); ++word) {
      if (word != 0U)
        stream << ':';
      stream << state.recent_matrix_cat_words[slot][word];
    }
  }
  stream << '"' << std::dec
         << ",\"recent_matrix_watch\":\"" << std::hex;
  const std::size_t watch_recent_count =
      (std::min)(state.recent_matrix_watch_position,
                 state.recent_matrix_watch_addresses.size());
  for (std::size_t index = 0U; index < watch_recent_count; ++index) {
    const std::size_t distance = watch_recent_count - index;
    const std::size_t slot =
        (state.recent_matrix_watch_position - distance) %
        state.recent_matrix_watch_addresses.size();
    if (index != 0U)
      stream << ',';
    stream << state.recent_matrix_watch_addresses[slot] << ':'
           << state.recent_matrix_watch_before[slot] << ':'
           << state.recent_matrix_watch_after[slot] << ':'
           << state.recent_matrix_watch_targets[slot] << ':'
           << state.recent_matrix_watch_callers[slot] << ':'
           << state.recent_matrix_watch_current_targets[slot] << ':'
           << state.recent_matrix_watch_return_addresses[slot];
  }
  stream << '"' << std::dec
         << ",\"poison_matrix_watch\":\"" << std::hex;
  for (std::size_t index = 0U; index < state.poison_matrix_watch.size();
       ++index) {
    if (index != 0U)
      stream << ':';
    stream << state.poison_matrix_watch[index];
  }
  stream << '"' << std::dec
         << ",\"poison_dispatch_history\":\"" << std::hex;
  for (std::size_t index = 0U; index < state.poison_dispatch_history.size();
       ++index) {
    if (index != 0U)
      stream << ',';
    stream << state.poison_dispatch_history[index];
  }
  stream << '"' << std::dec
         << ",\"materialize_reject_reason\":"
         << state.materialize_reject_reason
         << ",\"materialize_reject_address\":\"0x" << std::hex
         << state.materialize_reject_address << "\""
         << ",\"materialize_reject_word0\":\"0x"
         << state.materialize_reject_word0 << "\""
         << ",\"materialize_reject_word1\":\"0x"
         << state.materialize_reject_word1 << "\"" << std::dec
         << ",\"materialize_parent_address\":\"0x" << std::hex
         << state.materialize_parent_address << "\""
         << ",\"materialize_parent_word0\":\"0x"
         << state.materialize_parent_word0 << "\""
         << ",\"materialize_parent_word1\":\"0x"
         << state.materialize_parent_word1 << "\"" << std::dec
         << ",\"materialize_block_address\":\"0x" << std::hex
         << state.materialize_block_address << "\""
         << ",\"materialize_parent_segment_base\":\"0x"
         << state.materialize_parent_segment_base << "\"" << std::dec
         << ",\"recent_dispatches\":\"";
  const std::size_t recent_count =
      (std::min)(state.recent_dispatch_position,
                 state.recent_dispatches.size());
  for (std::size_t index = 0U; index < recent_count; ++index) {
    const std::size_t distance = recent_count - index;
    if (index != 0U)
      stream << ',';
    stream << std::hex
           << state.recent_dispatches[(state.recent_dispatch_position -
                                       distance) %
                                      state.recent_dispatches.size()];
  }
  stream << std::dec << "\"}\n";
  (void)state.snapshot_writer.submit(0U,{{state.progress_path,
      std::make_shared<const std::string>(stream.str())}});
  // Keep the replay and timing streams recoverable even if the host console
  // or launcher disappears without giving the runtime a normal shutdown.
  // This runs on the existing half-second progress cadence, off the
  // per-frame hot path used by the renderer diagnostics.
  if (state.input_record.is_open())
    state.input_record.flush();
  if (state.timing_trace.is_open())
    state.timing_trace.flush();
  state.gameplay_trace.flush();
  if (state.retrace_hash_trace.is_open())
    state.retrace_hash_trace.flush();
  if (state.update_hash_trace.is_open())
    state.update_hash_trace.flush();
  if (state.update_word_trace.is_open())
    state.update_word_trace.flush();
  if (state.controller_return_trace.is_open())
    state.controller_return_trace.flush();
  if (state.poll_hash_trace.is_open())
    state.poll_hash_trace.flush();
  if (state.event_trace.is_open())
    state.event_trace.flush();
}
#endif

#if defined(JFG_PHASE8_LIVE_RUNTIME)
bool configure_live_vi_mode(State &state, hle::GuestMemory &memory,
                            const std::uint32_t address) {
  // libultra's OSViMode is 0x50 bytes: a padded type byte, nine common
  // registers at +0x04, then five registers for each of the two fields.
  std::array<std::uint32_t, 9U> common{};
  std::array<LiveViFieldRegisters, 2U> fields{};
  for (std::size_t index = 0U; index < common.size(); ++index) {
    if (!memory.read_u32(address + 4U +
                             static_cast<std::uint32_t>(index * 4U),
                         common[index]))
      return false;
  }
  for (std::size_t field = 0U; field < fields.size(); ++field) {
    const std::uint32_t base =
        address + 0x28U + static_cast<std::uint32_t>(field * 0x14U);
    if (!memory.read_u32(base, fields[field].origin) ||
        !memory.read_u32(base + 4U, fields[field].y_scale) ||
        !memory.read_u32(base + 8U, fields[field].vertical_start) ||
        !memory.read_u32(base + 12U, fields[field].vertical_burst) ||
        !memory.read_u32(base + 16U, fields[field].intr))
      return false;
  }
  const std::uint32_t pixel_type = common[0U] & 0x03U;
  if ((pixel_type != 2U && pixel_type != 3U) || common[1U] == 0U ||
      common[1U] > 4096U || (common[7U] & 0x0FFFU) == 0U ||
      fields[0U].origin >= kRdramSize ||
      fields[1U].origin >= kRdramSize ||
      (fields[0U].y_scale & 0x0FFFU) == 0U ||
      (fields[1U].y_scale & 0x0FFFU) == 0U)
    return false;

  state.vi_mode_control = common[0U];
  state.rt64_vi.status = common[0U];
  state.rt64_vi.width = common[1U];
  state.rt64_vi.timing = common[2U];
  state.rt64_vi.vertical_sync = common[3U];
  state.rt64_vi.horizontal_sync = common[4U];
  state.rt64_vi.leap = common[5U];
  state.vi_mode_horizontal_start = common[6U];
  state.rt64_vi.horizontal_start =
      state.vi_blacked ? 0U : state.vi_mode_horizontal_start;
  state.rt64_vi.x_scale = common[7U];
  state.rt64_vi.current_line = common[8U] & 0x03FFU;
  state.vi_fields = fields;
  state.rt64_vi.y_scale = fields[0U].y_scale;
  state.rt64_vi.vertical_start = fields[0U].vertical_start;
  state.rt64_vi.vertical_burst = fields[0U].vertical_burst;
  state.rt64_vi.intr = fields[0U].intr;
  state.vi_mode_configured = true;
  state.gameplay_trace.event("vi-mode", {state.vi_retraces, common[0U], common[1U],
      common[6U], fields[0U].vertical_start, common[7U], fields[0U].y_scale});
  return true;
}

bool apply_live_vi_field(State &state, const std::uint32_t framebuffer) {
  const bool serrate = (state.rt64_vi.status & 0x40U) != 0U;
  const std::size_t field = serrate
      ? static_cast<std::size_t>((state.rt64_vi.current_line >> 1U) & 1U)
      : 0U;
  const LiveViFieldRegisters &registers = state.vi_fields[field];
  const std::uint64_t origin =
      std::uint64_t{framebuffer & 0x00FF'FFFFU} + registers.origin;
  if (origin >= jfg::kRt64RequiredRdramBytes)
    return false;
  state.rt64_vi.origin = static_cast<std::uint32_t>(origin);
  state.rt64_vi.y_scale = registers.y_scale;
  state.rt64_vi.vertical_start = registers.vertical_start;
  state.rt64_vi.vertical_burst = registers.vertical_burst;
  state.rt64_vi.intr = registers.intr;
  return true;
}

bool configure_flash_path(State &state, const char *path) {
  if (path == nullptr || *path == '\0')
    return true;
  state.flash_path = std::filesystem::path(path);
  if (state.flash_path.empty() || state.flash_path.filename().empty())
    return false;
  std::error_code error;
  const std::filesystem::path parent = state.flash_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error)
      return false;
  }
  const bool exists = std::filesystem::exists(state.flash_path, error);
  if (error)
    return false;
  if (exists) {
    if (!std::filesystem::is_regular_file(state.flash_path, error) || error ||
        !state.flashram.reload(state.flash_path).ok())
      return false;
    state.flash_image_loaded = true;
    return true;
  }
  if (!state.flashram.persist_atomic(state.flash_path).ok())
    return false;
  state.flash_image_created = true;
  return true;
}

bool configure_controller_pak_path(State &state, const char *path) {
  if (path == nullptr || *path == '\0')
    return true;
  state.controller_pak_path = std::filesystem::path(path);
  if (state.controller_pak_path.empty() ||
      state.controller_pak_path.filename().empty())
    return false;
  std::error_code error;
  const std::filesystem::path parent =
      state.controller_pak_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error)
      return false;
  }
  state.controller_accessory_bus =
      std::make_unique<jfg::ControllerAccessoryBus>(
          jfg::ControllerPakGeometry{32U * 1024U, 16U});
  if (!state.controller_accessory_bus
           ->set_accessory(0U, jfg::ControllerAccessoryKind::controller_pak)
           .ok())
    return false;
  const jfg::ControllerPakProbeResult probe =
      state.controller_accessory_bus->probe_controller_pak(0U);
  if (!probe.ok())
    return false;
  state.controller_pak_session = probe.session;
  const bool exists = std::filesystem::exists(state.controller_pak_path, error);
  if (error)
    return false;
  if (exists) {
    if (!std::filesystem::is_regular_file(state.controller_pak_path, error) ||
        error ||
        !state.controller_accessory_bus
             ->reload(state.controller_pak_session, state.controller_pak_path)
             .ok())
      return false;
    state.controller_pak_loaded = true;
    return true;
  }
  if (!state.controller_accessory_bus
           ->persist(state.controller_pak_session, state.controller_pak_path)
           .ok())
    return false;
  state.controller_pak_created = true;
  return true;
}

bool persist_controller_pak(State &state) {
  return state.controller_pak_path.empty() ||
         (state.controller_accessory_bus != nullptr &&
          state.controller_accessory_bus
              ->persist(state.controller_pak_session,
                        state.controller_pak_path)
              .ok());
}

bool mirror_synthetic_overlay_in_runlink(
    State &state, const JfgGeneratedSectionMetadata &metadata) {
  if (state.rdram == nullptr)
    return false;
  hle::GuestMemory memory({state.rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  const RunlinkPublicationResult result = publish_runlink_module(
      memory, kRunlinkModuleTablePointer, kRunlinkOverlaySlotCount,
      RunlinkModuleIdentity{metadata.rom_start, metadata.text_size,
                            metadata.data_size, metadata.bss_size},
      metadata.linked_vram);
  return result == RunlinkPublicationResult::published ||
         result == RunlinkPublicationResult::already_published ||
         result == RunlinkPublicationResult::table_uninitialized;
}

enum class SyntheticOverlayPublication : std::uint8_t {
  ensure_active,
  reload,
};

bool ensure_guest_overlay_allocation(
    State &state, const JfgGeneratedSectionMetadata &metadata,
    recomp_context &caller_context) {
  hle::GuestMemory memory({state.rdram, kRdramSize},
                         hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t table = 0U;
  if (!memory.read_u32(kRunlinkModuleTablePointer, table))
    return false;
  if (table == 0U)
    return true; // The guest has not initialized its module inventory yet.
  // Validate the complete word-aligned inventory once. Its native-word
  // layout allows memcpy reads; inspect every identity on every call so
  // unloads, changed identities, duplicate entries and malformed tables
  // retain the checked path's behavior. No guest allocation is cached.
  const std::uint64_t first_record = std::uint64_t{table} + kRunlinkModuleRecordSize;
  const std::uint64_t inventory_end = std::uint64_t{table} +
      kRunlinkOverlaySlotCount * kRunlinkModuleRecordSize + 20U;
  if ((table & 3U) != 0U || first_record + 4U < kKseg0 ||
      inventory_end > std::uint64_t{kKseg0} + kRdramSize)
    return false;
  std::uint32_t matched_slot = 0U, matched_record = 0U;
  for (std::uint32_t slot = 1U; slot <= kRunlinkOverlaySlotCount; ++slot) {
    const auto record = table + slot * kRunlinkModuleRecordSize;
    std::array<std::uint32_t, 4U> identity{};
    std::memcpy(identity.data(), state.rdram + (record + 4U - kKseg0),
                sizeof(identity));
    if (identity[0] != metadata.rom_start || identity[1] != metadata.text_size ||
        identity[2] != metadata.data_size || identity[3] != metadata.bss_size)
      continue;
    if (matched_slot != 0U)
      return false;
    matched_slot = slot;
    matched_record = record;
  }
  if (matched_slot == 0U)
    return false;
  std::uint32_t base = 0U;
  if (!memory.read_u32(matched_record, base))
    return false;
  if (base != 0U && base != metadata.linked_vram)
    return true; // Preserve the allocation chosen by the guest loader.
  if (!state.guest_overlay_loads_in_progress.insert(matched_slot).second)
    return false;
  // A suspended module retains its data/BSS allocation. The cold loader
  // deliberately rejects its pending entry; resume through the generated
  // guest routine so it can reacquire text without resetting that state.
  const auto suspension = resolve_runlink_suspension(
      memory, 0x800FEAD8U, 16U, matched_slot);
  if (suspension == RunlinkSuspensionResult::invalid) {
    state.guest_overlay_loads_in_progress.erase(matched_slot);
    return false;
  }
  const bool resume = suspension == RunlinkSuspensionResult::suspended;
  auto *loader = jfg_generated_lookup_function(static_cast<std::int32_t>(
      resume ? 0x80053D2CU : 0x80052CFCU));
  if (loader == nullptr || !memory.write_u32(matched_record, 0U)) {
    state.guest_overlay_loads_in_progress.erase(matched_slot);
    return false;
  }
  // Use the real generated loader, including temporary relocation allocations,
  // PI completion and module lifetime state. Preserve the pending call's CPU
  // arguments; the loader uses the current guest stack below its incoming SP.
  recomp_context loader_context = caller_context;
  loader_context.r4 = matched_slot;
  loader(state.rdram, &loader_context);
  state.guest_overlay_loads_in_progress.erase(matched_slot);
  // Resume has a void guest ABI; its published base is the success condition.
  return (resume || static_cast<std::uint32_t>(loader_context.r2) != 0U) &&
         memory.read_u32(matched_record, base) && base != 0U &&
         base != metadata.linked_vram;
}

bool publish_synthetic_overlay(
    State &state, const std::uint32_t section,
    const JfgGeneratedSectionMetadata &metadata,
    const SyntheticOverlayPublication publication =
        SyntheticOverlayPublication::ensure_active) {
  const auto reject = [&state](const char *reason) {
    state.last_overlay_publication_failure = reason;
    return false;
  };
  state.last_overlay_publication_failure = "none";
  if (state.rdram == nullptr || state.rom == nullptr ||
      metadata.is_overlay != 1U)
    return reject("invalid-input");
  const bool was_active = state.active_overlay_sections.contains(section);
  if (was_active &&
      publication == SyntheticOverlayPublication::ensure_active) {
    if (!mirror_synthetic_overlay_in_runlink(state, metadata))
      return reject("active-runlink");
    return true;
  }
  const std::uint64_t initialized =
      std::uint64_t{metadata.text_size} + metadata.data_size;
  const std::uint64_t extent = initialized + metadata.bss_size;
  const std::uint32_t host_offset = metadata.linked_vram - kKseg0;
  if (initialized > SIZE_MAX || extent > SIZE_MAX || extent == 0U ||
      metadata.rom_start > state.rom_size ||
      initialized > state.rom_size - metadata.rom_start ||
      host_offset > kGuestAddressSpan ||
      extent > kGuestAddressSpan - host_offset)
    return reject("range");

  const std::size_t expected = jfg_generated_relocation_count(section);
  const std::uint32_t *sites = nullptr;
  const JfgGeneratedR32Descriptor *descriptors = nullptr;
  std::size_t site_count = 0U, descriptor_count = 0U;
  if (jfg_generated_relocation_sites(section, &sites, &site_count) == 0 ||
      jfg_generated_relocation_descriptors(section, &descriptors,
                                           &descriptor_count) == 0 ||
      site_count != expected || descriptor_count != expected ||
      (expected != 0U && (sites == nullptr || descriptors == nullptr)))
    return reject("relocation-table");

  std::vector<std::pair<std::size_t, std::uint32_t>> patches;
  patches.reserve(expected);
  std::unordered_set<std::uint32_t> unique_sites;
  for (std::size_t index = 0U; index < expected; ++index) {
    const auto &descriptor = descriptors[index];
    JfgGeneratedSectionMetadata target{};
    if (sites[index] != descriptor.site_offset ||
        !unique_sites.insert(descriptor.site_offset).second ||
        descriptor.site_offset > initialized ||
        initialized - descriptor.site_offset < sizeof(std::uint32_t) ||
        jfg_generated_section_metadata(descriptor.target_section, &target) ==
            0)
      return reject("relocation-descriptor");
    const std::uint64_t target_extent = std::uint64_t{target.text_size} +
                                        target.data_size + target.bss_size;
    // The normalized ABI assigns every overlay a stable synthetic address.
    // Cross-overlay R_MIPS_32 values must therefore be published even while
    // the target module is inactive: the game stores these as deferred
    // function/data references and loads the selected target later. Requiring
    // every referenced cutscene module to be active made the shared animation
    // overlay impossible to load at all.
    if (descriptor.target_offset >= target_extent)
      return reject("relocation-target");
    const std::uint64_t value =
        std::uint64_t{target.linked_vram} + descriptor.target_offset;
    if (value > UINT32_MAX)
      return reject("relocation-value");
    patches.emplace_back(static_cast<std::size_t>(host_offset) +
                             descriptor.site_offset,
                         static_cast<std::uint32_t>(value));
  }

  // Direct calls merely ensure that a normalized overlay is available. An
  // exact PI load of the module ROM start is different: it is the game's
  // genuine overlay load boundary. Honor the generated lifecycle inventory
  // there so mutable data and BSS (including the health-meter state in
  // section 6) cannot survive a death/respawn reload.
  if (was_active &&
      jfg_generated_section_lifecycle(
          2U, section, static_cast<std::int32_t>(metadata.linked_vram)) != 0)
    return reject("lifecycle-unload");

  auto *const destination = state.rdram + host_offset;
  for (std::size_t index = 0U; index < static_cast<std::size_t>(initialized);
       ++index)
    destination[index ^ 3U] = state.rom[metadata.rom_start + index];
  std::fill_n(destination + static_cast<std::size_t>(initialized),
              static_cast<std::size_t>(metadata.bss_size), 0U);
  if (jfg_generated_section_lifecycle(
          1U, section, static_cast<std::int32_t>(metadata.linked_vram)) != 0)
    return reject("lifecycle");
  for (const auto &[offset, value] : patches)
    std::memcpy(state.rdram + offset, &value, sizeof(value));
  if (!mirror_synthetic_overlay_in_runlink(state, metadata))
    return reject("runlink");
  state.active_overlay_sections.insert(section);
  return true;
}

#endif

bool resolve_bzero_host_offset(const State &state, const std::uint32_t address,
                               const std::uint32_t length,
                               std::size_t &host_offset) {
  const std::uint32_t segment = address & 0xE0000000U;
  const std::uint64_t physical = address & 0x1FFFFFFFU;
  if ((segment == 0x80000000U || segment == 0xA0000000U) &&
      physical <= kRdramSize && length <= kRdramSize - physical) {
    host_offset = static_cast<std::size_t>(physical);
    return true;
  }

#if defined(JFG_LIVE_RUNTIME)
  // Generated overlay-local references use their normalized synthetic VRAM
  // addresses. GuestBacking reserves and commits those exact windows at the
  // offset used by the generated MEM_* helpers, so HLE memory primitives must
  // recognize an active overlay's bounded window as well as ordinary RDRAM.
  for (const std::uint32_t section : state.active_overlay_sections) {
    JfgGeneratedSectionMetadata metadata{};
    if (jfg_generated_section_metadata(section, &metadata) == 0 ||
        metadata.is_overlay != 1U)
      continue;
    const std::uint64_t extent = std::uint64_t{metadata.text_size} +
                                 metadata.data_size + metadata.bss_size;
    const std::uint64_t begin = metadata.linked_vram;
    const std::uint64_t requested = address;
    if (requested < begin || requested - begin > extent ||
        length > extent - (requested - begin))
      continue;
    const std::uint64_t synthetic_host_offset =
        std::uint64_t{0x80000000U} + requested;
    if (synthetic_host_offset > kGuestAddressSpan ||
        length > kGuestAddressSpan - synthetic_host_offset)
      return false;
    host_offset = static_cast<std::size_t>(synthetic_host_offset);
    return true;
  }
#else
  (void)state;
#endif
  return false;
}

#if defined(JFG_PHASE8_LIVE_RUNTIME)
class LiveAudioMemory final : public jfg::AudioMemoryAccess {
public:
  explicit LiveAudioMemory(std::uint8_t *rdram) : rdram_(rdram) {}

  [[nodiscard]] bool read(const std::size_t region_index,
                          const std::size_t offset,
                          const std::span<std::byte> destination) noexcept
      override {
    if (region_index != 0U || destination.empty() || offset > kRdramSize ||
        destination.size() > kRdramSize - offset)
      return false;
    for (std::size_t index = 0U; index < destination.size(); ++index)
      destination[index] =
          static_cast<std::byte>(rdram_[(offset + index) ^ 3U]);
    return true;
  }

  [[nodiscard]] bool write_region(
      const std::size_t region_index, const std::size_t offset,
      const std::span<const std::byte> source) noexcept override {
    if (region_index != 0U || source.empty() || offset > kRdramSize ||
        source.size() > kRdramSize - offset)
      return false;
    for (std::size_t index = 0U; index < source.size(); ++index)
      rdram_[(offset + index) ^ 3U] =
          static_cast<std::uint8_t>(source[index]);
    ++write_operations_;
    written_bytes_ += source.size();
    return true;
  }

  [[nodiscard]] bool write_output(
      std::size_t, std::span<const std::byte>) noexcept override {
    return false;
  }

  [[nodiscard]] std::size_t write_operations() const noexcept {
    return write_operations_;
  }
  [[nodiscard]] std::size_t written_bytes() const noexcept {
    return written_bytes_;
  }

private:
  std::uint8_t *rdram_ = nullptr;
  std::size_t write_operations_ = 0U;
  std::size_t written_bytes_ = 0U;
};

bool guest_task_blob(const State &state, const std::uint32_t address,
                     const std::size_t size,
                     std::vector<std::byte> &output) {
  const std::uint32_t physical = address & 0x1FFFFFFFU;
  if (size == 0U || physical > kRdramSize || size > kRdramSize - physical)
    return false;
  output.resize(size);
  for (std::size_t index = 0U; index < size; ++index) {
    output[index] =
        static_cast<std::byte>(state.rdram[(physical + index) ^ 3U]);
  }
  return true;
}

std::uint32_t translate_live_graphics_address(
    const State &state, const std::uint32_t address) noexcept {
  // The single-range translator always preserves real RDRAM aliases. Check
  // that invariant once instead of rechecking it for every overlay range.
  const auto segment = address & 0xE0000000U;
  if ((segment == 0U || segment == 0x80000000U || segment == 0xA0000000U) &&
      (address & 0x1FFFFFFFU) < kRdramSize)
    return address;
  for (const LiveGraphicsOverlayShadow &shadow :
       state.graphics_overlay_shadows) {
    const std::uint32_t translated = jfg::translate_rt64_overlay_address(
        address, static_cast<std::uint32_t>(kRdramSize),
        {shadow.linked_base, shadow.extent, shadow.physical_base});
    if (translated != address)
      return translated;
  }
  return address;
}

bool materialize_live_graphics_overlays(
    State &state, std::vector<std::byte> &snapshot,
    const std::uint32_t command_address,
    const std::uint32_t command_count,
    jfg::SnapshotChanges *const changes = nullptr) {
  state.materialize_reject_reason = 0U;
  state.materialize_reject_address = 0U;
  state.materialize_reject_word0 = 0U;
  state.materialize_reject_word1 = 0U;
  state.materialize_parent_address = 0U;
  state.materialize_parent_word0 = 0U;
  state.materialize_parent_word1 = 0U;
  state.materialize_block_address = 0U;
  state.materialize_parent_segment_base = 0U;
  const auto reject = [&state](const std::uint32_t reason,
                               const std::uint32_t address,
                               const std::uint32_t word0,
                               const std::uint32_t word1) {
    state.materialize_reject_reason = reason;
    state.materialize_reject_address = address;
    state.materialize_reject_word0 = word0;
    state.materialize_reject_word1 = word1;
    return false;
  };
  if (state.rom == nullptr ||
      snapshot.size() != jfg::kRt64RequiredRdramBytes || command_count == 0U)
    return reject(1U, command_address, 0U, 0U);
  if (state.graphics_overlay_initial_data.empty()) {
    for (const LiveGraphicsOverlayShadow &shadow :
         state.graphics_overlay_shadows) {
      JfgGeneratedSectionMetadata metadata{};
      if (jfg_generated_section_metadata(shadow.section, &metadata) == 0)
        return false;
      const std::uint64_t initialized =
          std::uint64_t{metadata.text_size} + metadata.data_size;
      if (
          initialized > shadow.extent || metadata.rom_start > state.rom_size ||
          initialized > state.rom_size - metadata.rom_start ||
          shadow.physical_base > snapshot.size() ||
          shadow.extent > snapshot.size() - shadow.physical_base)
        return false;
      for (std::size_t index = 0U; index < initialized; ++index)
        snapshot[(shadow.physical_base + index) ^ 3U] =
            static_cast<std::byte>(state.rom[metadata.rom_start + index]);
      std::fill_n(snapshot.begin() + shadow.physical_base + initialized,
                  metadata.bss_size, std::byte{0});
    }
    for (const LiveGraphicsOverlayShadow &shadow :
         state.graphics_overlay_shadows) {
      const JfgGeneratedR32Descriptor *descriptors = nullptr;
      std::size_t count = 0U;
      if (jfg_generated_relocation_descriptors(shadow.section, &descriptors,
                                               &count) == 0 ||
          (count != 0U && descriptors == nullptr))
        return false;
      for (std::size_t index = 0U; index < count; ++index) {
        JfgGeneratedSectionMetadata target{};
        if (jfg_generated_section_metadata(descriptors[index].target_section,
                                           &target) == 0 ||
            shadow.extent < 4U ||
            descriptors[index].site_offset > shadow.extent - 4U ||
            descriptors[index].target_offset >
                UINT32_MAX - target.linked_vram)
          return false;
        const std::uint32_t linked =
            target.linked_vram + descriptors[index].target_offset;
        const std::uint32_t translated =
            translate_live_graphics_address(state, linked);
        std::memcpy(snapshot.data() + shadow.physical_base +
                        descriptors[index].site_offset,
                    &translated, sizeof(translated));
      }
    }
    state.graphics_overlay_initial_data.assign(snapshot.begin() + kRdramSize,
                                                snapshot.end());
  } else {
    for (const auto &shadow : state.graphics_overlay_shadows) {
      if (shadow.physical_base < kRdramSize ||
          shadow.physical_base - kRdramSize > state.graphics_overlay_initial_data.size() ||
          shadow.extent > state.graphics_overlay_initial_data.size() -
                              (shadow.physical_base - kRdramSize))
        return reject(1U, command_address, 0U, 0U);
      std::copy_n(state.graphics_overlay_initial_data.data() +
                      (shadow.physical_base - kRdramSize),
                  shadow.extent, snapshot.data() + shadow.physical_base);
    }
  }

  // Planet meshes and other overlay-local graphics are generated into
  // writable DATA/BSS. Keep cached text and inactive-module fallbacks, but
  // submit the current CPU-owned data for every published allocation.
  for (const auto &shadow : state.graphics_overlay_shadows) {
    if (!state.active_overlay_sections.contains(shadow.section))
      continue;
    JfgGeneratedSectionMetadata metadata{};
    if (jfg_generated_section_metadata(shadow.section, &metadata) == 0 ||
        metadata.text_size > shadow.extent ||
        shadow.physical_base > snapshot.size() ||
        shadow.extent > snapshot.size() - shadow.physical_base)
      return reject(1U, shadow.linked_base, 0U, 0U);
    const std::uint32_t host_offset = shadow.linked_base - kKseg0;
    if (host_offset > kGuestAddressSpan ||
        shadow.extent > kGuestAddressSpan - host_offset)
      return reject(1U, shadow.linked_base, 0U, 0U);
    std::memcpy(snapshot.data() + shadow.physical_base + metadata.text_size,
                state.rdram + host_offset + metadata.text_size,
                shadow.extent - metadata.text_size);

    // Live pointer fields contain CPU addresses. Translate the current value,
    // preserving pointers that the game has changed since module loading.
    const std::uint32_t *sites = nullptr;
    std::size_t count = 0U;
    if (jfg_generated_relocation_sites(shadow.section, &sites, &count) == 0 ||
        (count != 0U && sites == nullptr))
      return reject(1U, shadow.linked_base, 0U, 0U);
    for (std::size_t index = 0U; index < count; ++index) {
      const std::uint32_t offset = sites[index];
      if (shadow.extent < sizeof(std::uint32_t) ||
          offset > shadow.extent - sizeof(std::uint32_t))
        return reject(1U, shadow.linked_base, 0U, 0U);
      if (offset < metadata.text_size)
        continue;
      auto *const site = snapshot.data() + shadow.physical_base + offset;
      std::uint32_t value = 0U;
      std::memcpy(&value, site, sizeof(value));
      value = translate_live_graphics_address(state, value);
      std::memcpy(site, &value, sizeof(value));
    }
  }

  struct DisplayListFrame final {
    std::uint32_t address;
    std::uint32_t count;
    std::uint32_t index;
    std::uint32_t parent_address;
    std::uint32_t parent_word0;
    std::uint32_t parent_word1;
    std::uint32_t parent_segment_base;
    bool counted_dma;
  };
  std::array<bool, 16U> configured_segments{};
  std::array<std::uint32_t, 16U> segment_bases{};
  constexpr std::uint32_t kRt64DisplayListDmaMask =
      static_cast<std::uint32_t>(jfg::kRt64RequiredRdramBytes - 8U);
  const auto display_list_dma_address =
      [kRt64DisplayListDmaMask](const std::uint32_t address) {
    return address & kRt64DisplayListDmaMask;
  };
  constexpr std::uint32_t kUnboundedCommandLimit = 65536U;
  constexpr std::uint64_t kTraversalCommandLimit = 4U * 1024U * 1024U;
  std::vector<DisplayListFrame> frames{{
      display_list_dma_address(command_address), command_count, 0U,
      0U, 0U, 0U, 0U, false}};
  std::unordered_set<std::uint64_t> active_blocks;
  const auto block_key = [](const DisplayListFrame &frame) {
    return (std::uint64_t{frame.address} << 32U) |
           (std::uint64_t{frame.count} << 1U) |
           static_cast<std::uint64_t>(frame.counted_dma);
  };
  active_blocks.insert(block_key(frames.back()));
  std::uint64_t traversed_commands = 0U;
  while (!frames.empty()) {
    DisplayListFrame &frame = frames.back();
    state.materialize_parent_address = frame.parent_address;
    state.materialize_parent_word0 = frame.parent_word0;
    state.materialize_parent_word1 = frame.parent_word1;
    state.materialize_block_address = frame.address;
    state.materialize_parent_segment_base = frame.parent_segment_base;
    const std::uint32_t limit =
        frame.count == 0U ? kUnboundedCommandLimit : frame.count;
    if (frame.index >= limit) {
      if (frame.count == 0U)
        return reject(13U, frame.address, 0U, 0U);
      active_blocks.erase(block_key(frame));
      frames.pop_back();
      continue;
    }
    if (++traversed_commands > kTraversalCommandLimit)
      return reject(14U, frame.address, 0U, 0U);

    const std::uint64_t offset =
        std::uint64_t{frame.address} + frame.index * 8U;
    ++frame.index;
    if (offset > snapshot.size() || snapshot.size() - offset < 8U)
      return reject(10U, static_cast<std::uint32_t>(offset), 0U, 0U);
    std::uint32_t words[2]{};
    std::memcpy(words, snapshot.data() + offset, sizeof(words));
    const std::uint8_t opcode = static_cast<std::uint8_t>(words[0] >> 24U);

    // F3DDKR's counted DMA-list handler dispatches only RDP commands. Low
    // opcodes inside that fixed-size block are data/no-ops, not nested lists
    // or segment updates. Treating them as normal GBI commands caused the old
    // breadth-first walker to patch unrelated memory as display-list data.
    if (frame.counted_dma && (opcode & 0xC0U) != 0xC0U)
      continue;

    if (!frame.counted_dma && opcode == 0xBCU &&
        static_cast<std::uint8_t>(words[0]) == 0x06U) {
      const std::uint32_t segment = ((words[0] >> 8U) & 0xFFFFU) >> 2U;
      if (segment >= configured_segments.size())
        return reject(11U, static_cast<std::uint32_t>(offset), words[0],
                      words[1]);
      configured_segments[segment] = true;
      segment_bases[segment] = display_list_dma_address(
          translate_live_graphics_address(state, words[1]));
    }

    const bool address_command =
        (!frame.counted_dma &&
         (opcode == 0x01U || opcode == 0x02U || opcode == 0x03U ||
          opcode == 0x04U || opcode == 0x05U || opcode == 0x06U ||
          opcode == 0x07U)) ||
        opcode == 0xFDU || opcode == 0xFEU || opcode == 0xFFU;
    const std::uint32_t original_address = words[1];
    const std::uint32_t segment = original_address >> 24U;
    const std::uint32_t translated_address = address_command
        ? translate_live_graphics_address(state, original_address)
        : original_address;
    // A matched synthetic overlay pointer is absolute, even when its high
    // byte happens to name a configured RSP segment. The options overlay
    // links above 32 MiB; treating that address as segment 2 skips its RDP
    // state lists whenever the scene has installed a nonzero segment base.
    const std::uint32_t materialized_address = translated_address;
    if (address_command) {
      const bool overlay_payload_command =
          opcode == 0x01U || opcode == 0x02U || opcode == 0x03U ||
          opcode == 0x04U || opcode == 0x05U || opcode == 0xFDU;
      const bool translated_display_list_edge =
          (opcode == 0x06U || opcode == 0x07U) &&
          translated_address != original_address;
      if ((overlay_payload_command &&
           translated_address != original_address) ||
          translated_display_list_edge ||
          segment >= configured_segments.size() ||
          !configured_segments[segment])
        words[1] = materialized_address;
      // RGO host speed: a patched display-list word changes the snapshot.
      if (changes != nullptr &&
          std::memcmp(snapshot.data() + offset, words, sizeof(words)) != 0)
        changes->mark(static_cast<std::size_t>(offset), sizeof(words));
      std::memcpy(snapshot.data() + offset, words, sizeof(words));
    }

    if (!frame.counted_dma && (opcode == 0x06U || opcode == 0x07U)) {
      const std::uint32_t segment_base =
          segment < segment_bases.size() && configured_segments[segment]
              ? segment_bases[segment]
              : 0U;
      const std::uint32_t traversal_address = jfg::resolve_rt64_display_list_edge(
          original_address, materialized_address, segment_base);
      const bool counted_dma = opcode == 0x07U;
      const std::uint32_t nested_count =
          counted_dma ? ((words[0] >> 16U) & 0xFFU) : 0U;
      if (counted_dma &&
          (nested_count == 0U ||
           (words[0] & 0xFFFFU) != nested_count * 8U))
        return reject(12U, static_cast<std::uint32_t>(offset), words[0],
                      words[1]);

      // G_DL with the no-push flag is a branch: the parent never resumes.
      if (!counted_dma && ((words[0] >> 16U) & 1U) != 0U) {
        active_blocks.erase(block_key(frames.back()));
        frames.pop_back();
      }
      const DisplayListFrame nested{
          traversal_address, nested_count, 0U,
          static_cast<std::uint32_t>(offset), words[0], words[1],
          segment_base, counted_dma};
      const std::uint64_t key = block_key(nested);
      if (!active_blocks.insert(key).second)
        return reject(15U, traversal_address, words[0], words[1]);
      frames.push_back(nested);
    } else if (!frame.counted_dma && opcode == 0xB8U) {
      active_blocks.erase(block_key(frames.back()));
      frames.pop_back();
    }
  }
  return true;
}

bool execute_live_audio_task(State &state, hle::GuestMemory &memory) {
  std::array<std::byte, 64U> descriptor{};
  for (std::size_t offset = 0U; offset < descriptor.size(); offset += 4U) {
    std::uint32_t word = 0U;
    if (!memory.read_u32(state.loaded_rsp_task +
                             static_cast<std::uint32_t>(offset),
                         word))
      return false;
    descriptor[offset] = static_cast<std::byte>(word >> 24U);
    descriptor[offset + 1U] = static_cast<std::byte>(word >> 16U);
    descriptor[offset + 2U] = static_cast<std::byte>(word >> 8U);
    descriptor[offset + 3U] = static_cast<std::byte>(word);
  }
  std::uint32_t program_address = 0U, program_size = 0U,
                data_address = 0U, data_size = 0U,
                commands_address = 0U, commands_size = 0U;
  if (!memory.read_u32(state.loaded_rsp_task + 0x10U, program_address) ||
      !memory.read_u32(state.loaded_rsp_task + 0x14U, program_size) ||
      !memory.read_u32(state.loaded_rsp_task + 0x18U, data_address) ||
      !memory.read_u32(state.loaded_rsp_task + 0x1CU, data_size) ||
      !memory.read_u32(state.loaded_rsp_task + 0x30U, commands_address) ||
      !memory.read_u32(state.loaded_rsp_task + 0x34U, commands_size) ||
      program_size == 0U ||
      program_size > jfg::kMaximumAudioActiveProgramBytes ||
      data_size == 0U || data_size > jfg::kMaximumAudioProgramDataBytes ||
      commands_size == 0U ||
      commands_size > jfg::kMaximumAudioCommandStreamBytes ||
      program_size % jfg::kAudioInstructionAlignmentBytes != 0U ||
      commands_size % jfg::kAudioCommandAlignmentBytes != 0U)
    return false;
  std::vector<std::byte> program, program_data, commands;
  if (!guest_task_blob(state, program_address, program_size, program) ||
      !guest_task_blob(state, data_address, data_size, program_data) ||
      !guest_task_blob(state, commands_address, commands_size, commands))
    return false;
  std::unique_ptr<jfg::evidence::G2PrivateAudioProgram> generated_program(
      jfg_g2_private_audio_program_v2(
          {descriptor, program, program_data, 0U, 4U}));
  if (!generated_program)
    return false;
  LiveAudioMemory live_memory(state.rdram);
  const jfg::AudioBackendTaskView task{
      jfg::AudioProgramVariant::primary,
      program,
      program_data,
      commands,
      1U,
      1U,
      4U,
  };
  const jfg::AudioRspProgramReport report =
      generated_program->execute(task, live_memory);
  if (report.exit_reason != jfg::AudioRspExitReason::broke ||
      !report.parse_complete ||
      report.selected_variant != jfg::AudioProgramVariant::primary ||
      report.unsupported_command_count != 0U ||
      report.parsed_command_count !=
          commands.size() / jfg::kAudioCommandAlignmentBytes ||
      report.input_read_operation_count == 0U ||
      report.region_write_operation_count != live_memory.write_operations() ||
      report.region_bytes_written != live_memory.written_bytes())
    return false;
  ++state.decoded_audio_tasks;
  return true;
}

#if defined(_WIN32) || defined(JFG_HEADLESS_LIVE)
bool initialize_live_renderer(State &state, LiveRt64Window &window) {
  if (state.rdram == nullptr || state.rom == nullptr ||
      state.rom_size < jfg::kRt64RequiredHeaderBytes)
    return false;
#if defined(_WIN32)
  if (window.get() == nullptr)
    return false;
#endif
  state.rt64_rdram.assign(jfg::kRt64RequiredRdramBytes, std::byte{0});
  std::copy_n(reinterpret_cast<const std::byte *>(state.rdram), kRdramSize,
              state.rt64_rdram.begin());
  state.rt64_vi.status = 0x0000320EU;
  state.rt64_vi.width = 320U;
  state.rt64_vi.intr = 2U;
  state.rt64_vi.timing = 0x03E52239U;
  state.rt64_vi.vertical_sync = 525U;
  state.rt64_vi.horizontal_sync = 0x00000C15U;
  state.rt64_vi.leap = 0x0C150C15U;
  state.rt64_vi.horizontal_start = 0x006C02ECU;
  state.rt64_vi.vertical_start = 0x002501FFU;
  state.rt64_vi.vertical_burst = 0x000E0204U;
  state.rt64_vi.x_scale = 0x00000200U;
  state.rt64_vi.y_scale = 0x00000400U;
  jfg::Rt64ShellError error = jfg::Rt64ShellError::none;
#if defined(JFG_HEADLESS_LIVE)
  refresh_video_settings(state);
  jfg::Rt64ShellConfiguration shell_configuration{window.get(), window.thread_id(),
       std::span<const std::byte>(
           reinterpret_cast<const std::byte *>(state.rom),
           jfg::kRt64RequiredHeaderBytes),
       state.rt64_rdram, &state.rt64_vi, false,
       jfg::Rt64MemoryLayout::host_word_swapped, true, true};
  state.rt64_shell = jfg::Rt64Shell::create(shell_configuration, error);
  state.last_rt64_error = error;
  return state.rt64_shell != nullptr && error == jfg::Rt64ShellError::none;
#else
  const int saved_stdout = _dup(_fileno(stdout));
  HANDLE null_handle = CreateFileW(
      L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const int null_stdout =
      null_handle == INVALID_HANDLE_VALUE
          ? -1
          : _open_osfhandle(reinterpret_cast<std::intptr_t>(null_handle),
                            _O_WRONLY);
  if (null_stdout < 0 && null_handle != INVALID_HANDLE_VALUE)
    CloseHandle(null_handle);
  if (saved_stdout < 0 || null_stdout < 0 ||
      _dup2(null_stdout, _fileno(stdout)) != 0) {
    if (saved_stdout >= 0)
      _close(saved_stdout);
    if (null_stdout >= 0)
      _close(null_stdout);
    return false;
  }
  // RGO: MSAA is fixed when the renderer starts, so read the settings first.
  refresh_video_settings(state);
  jfg::Rt64ShellConfiguration shell_configuration{window.get(), window.thread_id(),
       std::span<const std::byte>(
           reinterpret_cast<const std::byte *>(state.rom),
           jfg::kRt64RequiredHeaderBytes),
       state.rt64_rdram, &state.rt64_vi, false,
       // Software overlays read and modify completed color pixels.
       // RT64 already completes its RAM readback before DP completion.
       jfg::Rt64MemoryLayout::host_word_swapped, true, true};
  shell_configuration.msaa_samples = state.video_managed ? state.video.msaa : 1U;
  state.rt64_shell = jfg::Rt64Shell::create(shell_configuration, error);
  std::fflush(stdout);
  (void)_dup2(saved_stdout, _fileno(stdout));
  _close(null_stdout);
  _close(saved_stdout);
  state.last_rt64_error = error;
  return state.rt64_shell != nullptr && error == jfg::Rt64ShellError::none;
#endif
}

constexpr bool is_immediate_offscreen_task_candidate(
    const std::uint32_t command_size) noexcept {
  // Both observed tiny actor passes finish within the same VI in BizHawk.
  // The renderer still verifies that their color target is not displayed.
  return command_size == 0x1BU || command_size == 0x1FU;
}

bool execute_live_graphics_task(State &state, hle::GuestMemory &memory) {
  if (state.rt64_shell == nullptr ||
      state.rt64_rdram.size() != jfg::kRt64RequiredRdramBytes)
    return false;
  std::uint32_t ucode_address = 0U, ucode_size = 0U,
                ucode_data_address = 0U, ucode_data_size = 0U,
                command_address = 0U, command_size = 0U;
  if (!memory.read_u32(state.loaded_rsp_task + 0x10U, ucode_address) ||
      !memory.read_u32(state.loaded_rsp_task + 0x14U, ucode_size) ||
      !memory.read_u32(state.loaded_rsp_task + 0x18U, ucode_data_address) ||
      !memory.read_u32(state.loaded_rsp_task + 0x1CU, ucode_data_size) ||
      !memory.read_u32(state.loaded_rsp_task + 0x30U, command_address) ||
      !memory.read_u32(state.loaded_rsp_task + 0x34U, command_size))
    return false;
  state.last_graphics_descriptor = {ucode_address, ucode_size,
                                    ucode_data_address, ucode_data_size,
                                    command_address, command_size};
  const auto physical_range = [](const std::uint32_t address,
                                 const std::uint32_t bytes) {
    const std::uint32_t physical = address & 0x00FFFFFFU;
    return physical <= kRdramSize && bytes <= kRdramSize - physical;
  };
  // libultra does not require graphics tasks to populate ucode_size, and the
  // game uses data_size as a display-list count for some tiny setup tasks.
  // RT64 consumes the addresses and terminates on the GBI end command, so
  // validate the fixed apertures that its shell will inspect instead.
  if (!physical_range(ucode_address, 4096U) ||
      !physical_range(ucode_data_address, 2048U) ||
      !physical_range(command_address, sizeof(std::uint64_t)))
    return false;
  state.last_graphics_command_address = command_address;
  if (!memory.read_u32(command_address, state.last_graphics_command_word))
    return false;
  std::vector<std::byte> snapshot;
  std::uint64_t base_id = 0U;
  if (!state.graphics_snapshot_pool.empty()) {
    snapshot = std::move(state.graphics_snapshot_pool.back().rdram);
    base_id = state.graphics_snapshot_pool.back().id;
    state.graphics_snapshot_pool.pop_back();
  }
  if (snapshot.size() != jfg::kRt64RequiredRdramBytes)
    base_id = 0U;  // a new buffer holds no earlier snapshot
  snapshot.resize(jfg::kRt64RequiredRdramBytes);
  PendingLiveGraphicsTask pending;
  pending.task = {ucode_address, ucode_data_address, command_address};
  pending.descriptor = state.last_graphics_descriptor;
  pending.rdram = std::move(snapshot);
  pending.snapshot_id = ++state.next_snapshot_id;
  pending.base_id = base_id;
  pending.changes.reset(jfg::kRt64RequiredRdramBytes);
  jfg::copy_changed_snapshot_bytes(std::span(reinterpret_cast<const std::byte *>(state.rdram), kRdramSize),
      std::span(pending.rdram).first(kRdramSize), &pending.changes);
  if (!materialize_live_graphics_overlays(
          state, pending.rdram, command_address, command_size, &pending.changes)) {
    (void)write_private_rdram_capture(state);
    (void)write_private_rt64_snapshot(state, pending.rdram);
    return false;
  }
  state.pending_graphics_tasks.push_back(std::move(pending));
  return true;
}
#endif
#endif

void hash_u32(jfg::testkernel::Sha256 &hasher, const std::uint32_t value) {
  const std::array<std::uint8_t, 4U> bytes = {
      static_cast<std::uint8_t>(value >> 24U),
      static_cast<std::uint8_t>(value >> 16U),
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value)};
  hasher.update_bytes(bytes.data(), bytes.size());
}

void hash_u64(jfg::testkernel::Sha256 &hasher, const std::uint64_t value) {
  hash_u32(hasher, static_cast<std::uint32_t>(value >> 32U));
  hash_u32(hasher, static_cast<std::uint32_t>(value));
}

std::string journal_hash(const State &state) {
  jfg::testkernel::Sha256 hasher;
  static constexpr std::string_view domain = "jfg-phase6-native-journal-v1";
  hasher.update_bytes(domain.data(), domain.size());
  hash_u64(hasher, state.journal.count);
  for (std::size_t index = 0U; index < state.journal.count; ++index) {
    const NativeEvent &event = state.journal.entries[index];
    hash_u32(hasher, static_cast<std::uint32_t>(event.kind));
    hash_u32(hasher, event.a);
    hash_u32(hasher, event.b);
    hash_u32(hasher, event.c);
  }
  return jfg::testkernel::hex_digest(hasher.finish());
}

std::string mmio_trace_text(const State &state) {
  std::string trace;
  for (std::size_t index = 0U; index < state.journal.count; ++index) {
    const NativeEvent &event = state.journal.entries[index];
    if (event.kind != NativeEventKind::kMmioRead &&
        event.kind != NativeEventKind::kMmioWrite)
      continue;
    if (!trace.empty())
      trace.push_back(',');
    char entry[13U]{};
    (void)std::snprintf(entry, sizeof(entry), "%c:0x%08x",
                        event.kind == NativeEventKind::kMmioWrite ? 'w' : 'r',
                        event.a);
    trace += entry;
  }
  return trace;
}

std::string state_hash(const State &state) {
  jfg::testkernel::Sha256 hasher;
  static constexpr std::string_view domain = "jfg-phase6-native-state-v1";
  hasher.update_bytes(domain.data(), domain.size());
  hasher.update_bytes(state.rdram, kRdramSize);
  hash_u32(hasher, state.root_priority);
  hash_u32(hasher, state.interrupt_mask);
  hash_u32(hasher, state.vi_mode);
  hash_u32(hasher, state.vi_queue);
  hash_u32(hasher, state.vi_message);
  hash_u32(hasher, state.vi_retrace_interval);
  hash_u32(hasher, state.vi_current_framebuffer);
  hash_u32(hasher, state.vi_next_framebuffer);
  hash_u32(hasher, state.vi_special_features);
  hash_u32(hasher, state.vi_blacked ? 1U : 0U);
  hash_u64(hasher, state.vi_frames);
  hash_u64(hasher, state.vi_interrupts);
  hash_u64(hasher, state.vi_messages_delivered);
  hash_u64(hasher, state.vi_retraces);
  hash_u64(hasher, state.scheduler == nullptr
                       ? 0U
                       : state.scheduler->dispatch_count());
  hash_u64(hasher, state.thread_order.size());
  for (const auto &[guest, id] : state.thread_order) {
    hash_u32(hasher, guest);
    hash_u32(hasher, static_cast<std::uint32_t>(id));
    const auto snapshot = state.scheduler->snapshot_of(id);
    hash_u32(hasher, snapshot.has_value() ? snapshot->priority : 0U);
    hash_u32(hasher, snapshot.has_value()
                         ? static_cast<std::uint32_t>(snapshot->state)
                         : 0U);
    hash_u32(hasher, snapshot.has_value() ? snapshot->blocked_queue : 0U);
  }
  return jfg::testkernel::hex_digest(hasher.finish());
}

void ledger(State &state, std::string_view category, std::string_view operation,
            std::uint32_t guest_target) {
  if (!state.ledgered) {
    state.ledgered = true;
    jfg::support_event("failure=" + std::string(category) + "/" + std::string(operation));
    std::fprintf(stdout,
                 "{\"kind\":\"jfg-phase6-native-first-trap\",\"category\":\"%.*"
                 "s\",\"guest_target\":\"0x%08x\",\"operation\":\"%.*s\","
                 "\"disposition\":\"fail-closed-trap\",\"count\":1}\n",
                 static_cast<int>(category.size()), category.data(),
                 guest_target, static_cast<int>(operation.size()),
                 operation.data());
    std::fflush(stdout);
  }
}

#if defined(JFG_LIVE_RUNTIME)
#if defined(_WIN32)
LONG WINAPI child_exception_filter(EXCEPTION_POINTERS *exception) {
  jfg::support_crash(exception);
  State *const state = g_active_child_state;
  if (state != nullptr) {
    const std::uint32_t target =
        state->recent_dispatch_position == 0U
            ? 0U
            : state->recent_dispatches[(state->recent_dispatch_position - 1U) %
                                       state->recent_dispatches.size()];
    const DWORD code =
        exception != nullptr && exception->ExceptionRecord != nullptr
            ? exception->ExceptionRecord->ExceptionCode
            : 0U;
    char support_exception[40]{};
    std::snprintf(support_exception, sizeof(support_exception), "native_exception=0x%08lx", static_cast<unsigned long>(code));
    jfg::support_event(support_exception);
    if (state->active_bzero_address != 0U) {
      std::array<char, 64U> operation{};
      (void)std::snprintf(operation.data(), operation.size(),
                          "bzero-%08x-%08x", state->active_bzero_address,
                          state->active_bzero_length);
      ledger(*state, "native-crash", operation.data(), target);
    } else {
      std::array<char, 64U> operation{};
      const auto module = reinterpret_cast<std::uintptr_t>(
          GetModuleHandleW(nullptr));
      const auto instruction =
          exception != nullptr && exception->ExceptionRecord != nullptr
              ? reinterpret_cast<std::uintptr_t>(
                    exception->ExceptionRecord->ExceptionAddress)
              : 0U;
      (void)std::snprintf(operation.data(), operation.size(),
                          code == EXCEPTION_ACCESS_VIOLATION
                              ? "access-rva-%08llx"
                              : "exception-rva-%08llx",
                          static_cast<unsigned long long>(instruction - module));
      ledger(*state, "native-crash", operation.data(), target);
    }
  }
  std::_Exit(4);
}
#else
// Headless: report the recent guest calls before a native crash ends the
// process (the Windows build writes the same ledger from its filter).
void headless_crash_report(const int signal_number) noexcept {
  State *const state = g_active_child_state;
  if (state == nullptr)
    return;
  const std::size_t count = (std::min)(state->recent_dispatch_position,
                                       state->recent_dispatches.size());
  std::fprintf(stderr, "native crash: signal %d at VI %llu; recent guest calls:",
               signal_number, static_cast<unsigned long long>(state->vi_retraces));
  for (std::size_t index = count; index > 0U; --index)
    std::fprintf(stderr, " %08x",
                 state->recent_dispatches[(state->recent_dispatch_position - index) %
                                          state->recent_dispatches.size()]);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::array<char, 32U> operation{};
  (void)std::snprintf(operation.data(), operation.size(), "signal-%d", signal_number);
  ledger(*state, "native-crash", operation.data(),
         count == 0U ? 0U : state->recent_dispatches[(state->recent_dispatch_position - 1U) %
                                                     state->recent_dispatches.size()]);
}
#endif
#endif

[[noreturn]] void fail_closed_dispatch(State &state, std::string_view category,
                                       std::string_view operation,
                                       std::uint32_t guest_target) {
#if defined(JFG_LIVE_RUNTIME)
  if (state.input_record.is_open())
    state.input_record.flush();
  if (state.timing_trace.is_open())
    state.timing_trace.flush();
  state.gameplay_trace.flush();
  if (!state.rdram_capture_path.empty())
    (void)write_private_rdram_capture(state);
#endif
  ledger(state, category, operation, guest_target);
  std::_Exit(4);
}

[[noreturn]] void stable_vi_gate(State &state) {
  if (state.journal.overflow || state.mmio_trace == nullptr ||
      state.mmio_trace->unsupported_accesses != 0U ||
      state.vi_interrupts < state.vi_retraces ||
      state.vi_messages_delivered < state.vi_retraces) {
    fail_closed_dispatch(state, "native", "invalid-stable-gate", 0U);
  }
  const std::string state_digest = state_hash(state);
  const std::string journal_digest = journal_hash(state);
  const std::string mmio_trace = mmio_trace_text(state);
  std::fprintf(stdout,
               "{\"kind\":\"jfg-phase6-native-boot\",\"status\":\"stable-vi\","
               "\"build_identity\":\"%s\",\"sanitizer\":\"%s\","
               "\"vi_retraces\":%llu,\"vi_frames\":%llu,"
               "\"vi_interrupts\":%llu,\"vi_messages_delivered\":%llu,"
               "\"vi_queue\":\"0x%08x\","
               "\"threads_created\":%zu,\"rdram_bytes\":%zu,"
               "\"mmio_accesses\":%llu,\"unsupported_accesses\":%llu,"
               "\"mmio_trace\":\"%s\","
               "\"journal_entries\":%zu,\"state_hash\":\"%s\","
               "\"journal_hash\":\"%s\"}\n",
               JFG_PHASE6_BUILD_IDENTITY, JFG_PHASE6_SANITIZER_ID,
               static_cast<unsigned long long>(state.vi_retraces),
               static_cast<unsigned long long>(state.vi_frames),
               static_cast<unsigned long long>(state.vi_interrupts),
               static_cast<unsigned long long>(state.vi_messages_delivered),
               state.vi_queue,
               state.threads.size(), kRdramSize,
               static_cast<unsigned long long>(state.mmio_trace->accesses),
               static_cast<unsigned long long>(
                   state.mmio_trace->unsupported_accesses),
               mmio_trace.c_str(), state.journal.count, state_digest.c_str(),
               journal_digest.c_str());
  std::fflush(stdout);
  state.host_audio.finish_capture(); state.timing_trace.finish(); state.guest_clock_trace.finish(); state.input_record.finish(); state.gameplay_trace.finish(); state.snapshot_writer.finish(); state.movement_trace.finish(); state.jump_trace.finish(); std::_Exit(0);
}

#if defined(JFG_PHASE8_LIVE_RUNTIME)
[[noreturn]] void probe_target_gate(State &state) {
  if (state.mmio_trace == nullptr)
    fail_closed_dispatch(state, "probe", "missing-mmio-trace", 0U);
  if (state.guest_os_probe) {
    if (state.poll_target != 0 || state.vi_retraces != state.retrace_target ||
        state.mmio_trace->unsupported_accesses != 0 || state.mmio_trace->device_error)
      fail_closed_dispatch(state, "guest-probe", "invalid-target", 0U);
    // Explicitly separate this diagnostic from legacy HLE scheduler evidence.
    // In particular, do not invent HLE thread snapshots for original OS threads.

    if (!service_host_audio(state,false) || !write_private_rdram_capture(state))
      fail_closed_dispatch(state,"guest-capture","final-state",0U);
    if (state.presented_frames != 0U && !write_live_frame_capture(state))
      fail_closed_dispatch(state,"guest-capture","final-image",0U);
    if (state.timing_trace) state.timing_trace << "fast-hook\t"
        << state.execution_fast_hits << '\t' << state.observed_guest_instructions << '\n';
    if (state.timing_trace) state.timing_trace << "original-os-health\t" << state.vi_frames << '\t'
        << state.vi_retraces << '\t' << state.presented_frames << '\t'
        << state.audio_device_underruns << '\t' << state.audio_device_overruns << '\t'
        << state.mmio_trace->unsupported_accesses << '\t' << state.observed_guest_instructions << '\n';
    state.retrace_hash_trace.flush(); state.update_hash_trace.flush();
    state.poll_hash_trace.flush(); state.guest_clock_trace.flush();
    state.timing_trace.flush();
    state.gameplay_trace.flush();
    std::fprintf(stdout,
        "{\"kind\":\"jfg-phase9-original-os-probe\",\"acceptance\":false,"
        "\"status\":\"retrace-target\",\"retrace_target\":%u,\"vi_retraces\":%llu,"
        "\"controller_samples\":%llu,\"completed_updates\":%llu,\"cpu_count\":%llu,"
        "\"instructions\":%llu,\"graphics_tasks\":%llu,\"decoded_audio_tasks\":%llu,"
        "\"unsupported_accesses\":0,\"runtime_snapshot_complete\":false,\"state_hash\":\"%s\"}\n",
        state.retrace_target, static_cast<unsigned long long>(state.vi_retraces),
        static_cast<unsigned long long>(state.controller_samples),
        static_cast<unsigned long long>(state.completed_game_updates),
        static_cast<unsigned long long>(state.cpu_count),
        static_cast<unsigned long long>(state.observed_guest_instructions),
        static_cast<unsigned long long>(state.graphics_tasks),
        static_cast<unsigned long long>(state.decoded_audio_tasks), state_hash(state).c_str());
    std::fflush(stdout); state.host_audio.finish_capture(); state.timing_trace.finish(); state.guest_clock_trace.finish(); state.input_record.finish(); state.gameplay_trace.finish(); state.snapshot_writer.finish(); state.movement_trace.finish(); state.jump_trace.finish(); std::_Exit(0);
  }
  const auto &event_counts = state.journal.observed_counts;
  std::string overlay_sequence;
  for (std::size_t index = 0U; index < state.journal.count; ++index) {
    const NativeEvent &event = state.journal.entries[index];
    if (event.kind != NativeEventKind::kOverlayPublished)
      continue;
    if (!overlay_sequence.empty())
      overlay_sequence.push_back(',');
    overlay_sequence += std::to_string(event.a);
  }
  std::string hle_counts;
  for (std::size_t index = 0U; index < kMappingCount; ++index) {
    if (state.mapped_call_counts[index] == 0U)
      continue;
    if (!hle_counts.empty())
      hle_counts.push_back(',');
    hle_counts += kMappings[index].name;
    hle_counts.push_back('=');
    hle_counts += std::to_string(state.mapped_call_counts[index]);
  }
  std::array<std::size_t, 4U> thread_states{};
  std::string thread_summary;
  if (state.scheduler != nullptr) {
    for (std::size_t thread_index = 0U;
         thread_index < state.thread_order.size(); ++thread_index) {
      const auto &[guest, id] = state.thread_order[thread_index];
      (void)guest;
      const auto snapshot = state.scheduler->snapshot_of(id);
      if (snapshot.has_value()) {
        const auto index = static_cast<std::size_t>(snapshot->state);
        if (index < thread_states.size())
          ++thread_states[index];
        if (!thread_summary.empty())
          thread_summary.push_back(',');
        thread_summary += std::to_string(id);
        thread_summary.push_back(':');
        thread_summary += std::to_string(snapshot->priority);
        thread_summary.push_back(':');
        thread_summary +=
            std::to_string(static_cast<std::uint32_t>(snapshot->state));
        thread_summary.push_back(':');
        char queue[11U]{};
        (void)std::snprintf(queue, sizeof(queue), "%08x",
                            snapshot->blocked_queue);
        thread_summary += queue;
        thread_summary.push_back(':');
        char entry[11U]{};
        (void)std::snprintf(
            entry, sizeof(entry), "%08x",
            thread_index < state.thread_entries.size()
                ? state.thread_entries[thread_index]
                : 0U);
        thread_summary += entry;
      }
    }
  }
  std::uint32_t si_queue = 0U, si_queue_valid = 0U;
  const auto si = state.events.find(5U);
  if (si != state.events.end()) {
    si_queue = si->second.first;
    hle::GuestMemory memory({state.rdram, kRdramSize},
                            hle::GuestMemory::Layout::native_word_big_endian);
    (void)memory.read_u32(si_queue + hle::kMqOffValidCount, si_queue_valid);
  }
  std::string si_dispatch_trace;
  for (const std::uint32_t dispatch_target : state.last_si_dispatch_trace) {
    if (!si_dispatch_trace.empty())
      si_dispatch_trace.push_back(',');
    char entry[11U]{};
    (void)std::snprintf(entry, sizeof(entry), "%08x", dispatch_target);
    si_dispatch_trace += entry;
  }
#if defined(JFG_LIVE_RUNTIME)
  if (!service_host_audio(state, false))
    fail_closed_dispatch(state, "audio", "host-device-final", 0U);
  if (!persist_controller_pak(state))
    fail_closed_dispatch(state, "save", "controller-pak-persist", 0U);
  if (!write_live_frame_capture(state))
    fail_closed_dispatch(state, "renderer", "frame-capture", 0U);
  if (!write_private_rdram_capture(state))
    fail_closed_dispatch(state, "runtime", "rdram-capture", 0U);
#endif
  const std::string state_digest = state_hash(state);
  const std::string journal_digest = journal_hash(state);
  const unsigned front_mode = state.rdram[0x000A51B0U ^ 3U];
  hle::GuestMemory probe_memory(
      {state.rdram, kRdramSize},
      hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t player_position_x = 0U;
  std::uint32_t player_position_y = 0U;
  std::uint32_t player_position_z = 0U;
  std::uint16_t player_yaw = 0U;
  if (state.phase9_player_actor != 0U) {
    std::uint32_t rotation_pair = 0U;
    if (probe_memory.read_u32(state.phase9_player_actor, rotation_pair))
      player_yaw = static_cast<std::uint16_t>(rotation_pair >> 16U);
    (void)probe_memory.read_u32(state.phase9_player_actor + 0x0CU,
                                player_position_x);
    (void)probe_memory.read_u32(state.phase9_player_actor + 0x10U,
                                player_position_y);
    (void)probe_memory.read_u32(state.phase9_player_actor + 0x14U,
                                player_position_z);
  }
  std::uint32_t hints_position_x = 0U;
  std::uint32_t hints_position_y = 0U;
  std::uint32_t hints_position_z = 0U;
  std::uint32_t hints_state = 0U;
  std::array<std::uint32_t, 12U> hints_state_words{};
  std::uint16_t hints_yaw = 0U;
  if (state.phase9_hints_actor != 0U) {
    std::uint32_t rotation_pair = 0U;
    if (probe_memory.read_u32(state.phase9_hints_actor, rotation_pair))
      hints_yaw = static_cast<std::uint16_t>(rotation_pair >> 16U);
    (void)probe_memory.read_u32(state.phase9_hints_actor + 0x0CU,
                                hints_position_x);
    (void)probe_memory.read_u32(state.phase9_hints_actor + 0x10U,
                                hints_position_y);
    (void)probe_memory.read_u32(state.phase9_hints_actor + 0x14U,
                                hints_position_z);
    if (probe_memory.read_u32(state.phase9_hints_actor + 0x68U,
                              hints_state) && hints_state != 0U) {
      for (std::size_t word = 0U; word < hints_state_words.size(); ++word)
        (void)probe_memory.read_u32(
            hints_state + static_cast<std::uint32_t>(word * 4U),
            hints_state_words[word]);
    }
  }
  std::uint32_t king_bear_actor = 0U;
  std::uint32_t object_list = 0U;
  std::uint32_t object_count = 0U;
  if (probe_memory.read_u32(0x800F2CA4U, object_list) &&
      probe_memory.read_u32(0x800F2CA8U, object_count) &&
      object_list != 0U && object_count <= 1024U) {
    for (std::uint32_t index = 0U; index < object_count; ++index) {
      std::uint32_t actor = 0U;
      std::uint32_t header = 0U;
      std::uint32_t name_word_0 = 0U;
      std::uint32_t name_word_1 = 0U;
      if (!probe_memory.read_u32(object_list + index * 4U, actor) ||
          actor == 0U ||
          !probe_memory.read_u32(actor + 0x40U, header) || header == 0U ||
          !probe_memory.read_u32(header + 4U, name_word_0) ||
          !probe_memory.read_u32(header + 8U, name_word_1))
        continue;
      if (name_word_0 == 0x4B696E67U && name_word_1 == 0x42656172U) {
        king_bear_actor = actor;
        break;
      }
    }
  }
  std::uint32_t king_bear_position_x = 0U;
  std::uint32_t king_bear_position_y = 0U;
  std::uint32_t king_bear_position_z = 0U;
  std::uint32_t king_bear_state = 0U;
  std::uint32_t king_bear_state_word_0 = 0U;
  std::uint32_t king_bear_sound_handle = 0U;
  std::uint32_t king_bear_state_word_30 = 0U;
  std::uint32_t king_bear_state_word_34 = 0U;
  std::uint16_t king_bear_yaw = 0U;
  if (king_bear_actor != 0U) {
    std::uint32_t rotation_pair = 0U;
    if (probe_memory.read_u32(king_bear_actor, rotation_pair))
      king_bear_yaw = static_cast<std::uint16_t>(rotation_pair >> 16U);
    (void)probe_memory.read_u32(king_bear_actor + 0x0CU,
                                king_bear_position_x);
    (void)probe_memory.read_u32(king_bear_actor + 0x10U,
                                king_bear_position_y);
    (void)probe_memory.read_u32(king_bear_actor + 0x14U,
                                king_bear_position_z);
    if (probe_memory.read_u32(king_bear_actor + 0x68U, king_bear_state) &&
        king_bear_state != 0U) {
      (void)probe_memory.read_u32(king_bear_state, king_bear_state_word_0);
      (void)probe_memory.read_u32(king_bear_state + 0x2CU,
                                  king_bear_sound_handle);
      (void)probe_memory.read_u32(king_bear_state + 0x30U,
                                  king_bear_state_word_30);
      (void)probe_memory.read_u32(king_bear_state + 0x34U,
                                  king_bear_state_word_34);
    }
  }
  std::uint32_t king_bear_callback_section = UINT32_MAX;
  std::uint32_t king_bear_callback_offset = 0U;
  if (state.phase9_king_bear_callback == 0x020002E8U) {
    king_bear_callback_section = 32U;
    king_bear_callback_offset = 0x2E8U;
  } else if (state.phase9_king_bear_callback != 0U) {
    RunlinkAddressResolution resolution{};
    if (resolve_runlink_text_address(
            probe_memory, kRunlinkModuleTablePointer,
            kRunlinkOverlaySlotCount, state.phase9_king_bear_callback,
            resolution) == RunlinkAddressResult::resolved) {
      for (std::uint32_t section = 0U;
           section < jfg_generated_section_count(); ++section) {
        JfgGeneratedSectionMetadata metadata{};
        if (jfg_generated_section_metadata(section, &metadata) != 0 &&
            metadata.is_overlay == 1U &&
            metadata.rom_start == resolution.identity.rom_start &&
            metadata.text_size == resolution.identity.text_size &&
            metadata.data_size == resolution.identity.data_size &&
            metadata.bss_size == resolution.identity.bss_size) {
          king_bear_callback_section = section;
          king_bear_callback_offset = resolution.text_offset;
          break;
        }
      }
    }
  }
  std::array<std::uint32_t, 8U> runlink_module7_words{};
  constexpr std::array<std::uint32_t, 16U> kHintTextStateOffsets{
      0x2A20U, 0x2A24U, 0x2A28U, 0x2A2CU,
      0x2A38U, 0x2A3CU, 0x2A40U, 0x2A44U,
      0x2A4CU, 0x2A58U, 0x2A5CU, 0x2A70U,
      0x2C30U, 0x2C34U, 0x2C3CU, 0x2C40U};
  const std::uint32_t hinttext_section_base =
      section_addresses != nullptr
          ? static_cast<std::uint32_t>(section_addresses[7U])
          : 0U;
  std::array<std::uint32_t, kHintTextStateOffsets.size()>
      hinttext_state_words{};
  if (hinttext_section_base != 0U) {
    const std::uint64_t sign_extended_base = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(
            static_cast<std::int32_t>(hinttext_section_base)));
    for (std::size_t word = 0U; word < hinttext_state_words.size(); ++word) {
      const std::uint64_t host_offset = sign_extended_base +
          kHintTextStateOffsets[word] - UINT64_C(0xFFFFFFFF80000000);
      if (host_offset <= kGuestAddressSpan - sizeof(std::uint32_t))
        std::memcpy(&hinttext_state_words[word], state.rdram + host_offset,
                    sizeof(std::uint32_t));
    }
  }
  std::uint32_t runlink_module_table = 0U;
  if (probe_memory.read_u32(kRunlinkModuleTablePointer,
                            runlink_module_table) &&
      runlink_module_table != 0U) {
    const std::uint32_t record = runlink_module_table +
        7U * kRunlinkModuleRecordSize;
    for (std::size_t word = 0U; word < runlink_module7_words.size(); ++word)
      (void)probe_memory.read_u32(
          record + static_cast<std::uint32_t>(word * sizeof(std::uint32_t)),
          runlink_module7_words[word]);
  }
  std::uint64_t weapon_stat_shots = 0U;
  std::uint64_t weapon_stat_hits = 0U;
  std::uint64_t weapon_stat_kills = 0U;
  std::uint32_t player_data = 0U;
  if (probe_memory.read_u32(0x800FD7D4U, player_data) && player_data != 0U) {
    for (std::uint32_t weapon = 0U; weapon < 16U; ++weapon) {
      std::uint32_t shots = 0U;
      std::uint32_t hits = 0U;
      std::uint32_t kills = 0U;
      const std::uint32_t slot = weapon * sizeof(std::uint32_t);
      if (probe_memory.read_u32(player_data + 0x474U + slot, shots))
        weapon_stat_shots += shots;
      if (probe_memory.read_u32(player_data + 0x4B4U + slot, hits))
        weapon_stat_hits += hits;
      if (probe_memory.read_u32(player_data + 0x434U + slot, kills))
        weapon_stat_kills += kills;
    }
  }
  bool emit_actor_trace = false;
#if defined(_WIN32)
  char *actor_trace = nullptr;
  std::size_t actor_trace_size = 0U;
  if (_dupenv_s(&actor_trace, &actor_trace_size, "JFG_PHASE9_ACTOR_TRACE") ==
          0 &&
      actor_trace != nullptr && std::strcmp(actor_trace, "1") == 0)
    emit_actor_trace = true;
  std::free(actor_trace);
#else
  if (const char *actor_trace = std::getenv("JFG_PHASE9_ACTOR_TRACE");
      actor_trace != nullptr && std::strcmp(actor_trace, "1") == 0)
    emit_actor_trace = true;
#endif
  if (emit_actor_trace) {
    std::uint32_t actor_list = 0U;
    std::uint32_t actor_count = 0U;
    if (probe_memory.read_u32(0x800F2CA4U, actor_list) &&
        probe_memory.read_u32(0x800F2CA8U, actor_count) &&
        actor_count <= 256U) {
      std::fprintf(stdout, "{\"kind\":\"jfg-phase9-actor-trace\","
                           "\"actors\":[");
      bool first_actor = true;
      for (std::uint32_t index = 0U; index < actor_count; ++index) {
        std::uint32_t actor = 0U;
        std::uint32_t header = 0U;
        std::uint32_t name0 = 0U;
        std::uint32_t name1 = 0U;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        std::uint32_t z = 0U;
        if (!probe_memory.read_u32(actor_list + index * 4U, actor) ||
            actor == 0U || !probe_memory.read_u32(actor + 0x40U, header) ||
            header == 0U || !probe_memory.read_u32(header + 4U, name0) ||
            !probe_memory.read_u32(header + 8U, name1) ||
            !probe_memory.read_u32(actor + 0x0CU, x) ||
            !probe_memory.read_u32(actor + 0x10U, y) ||
            !probe_memory.read_u32(actor + 0x14U, z))
          continue;
        if (!first_actor)
          std::fputc(',', stdout);
        first_actor = false;
        std::fprintf(stdout,
                     "{\"index\":%u,\"actor\":\"0x%08x\","
                     "\"name_words\":\"%08x,%08x\","
                     "\"position_words\":\"%08x,%08x,%08x\"}",
                     index, actor, name0, name1, x, y, z);
      }
      std::fputs("]}\n", stdout);
    }
  }
  // Host wall-clock totals are diagnostics, not simulation state. They live
  // in the timing side channel so the canonical probe record below is
  // byte-identical across deterministic runs.
  if (state.timing_trace) {
    state.timing_trace << "totals\t" << state.vi_retraces << '\t'
                       << state.audio_task_microseconds << '\t'
                       << state.graphics_prepare_microseconds << '\t'
                       << state.graphics_submit_microseconds << '\t'
                       << state.graphics_present_microseconds << '\n';
  }
  std::fprintf(
      stdout,
      "{\"kind\":\"jfg-phase8-native-probe\",\"status\":\"%s\","
      "\"%s\":%u,\"vi_retraces\":%llu,\"vi_frames\":%llu,"
      "\"vi_interrupts\":%llu,\"vi_messages_delivered\":%llu,"
      "\"mmio_accesses\":%llu,\"unsupported_accesses\":%llu,"
      "\"dispatches\":%zu,\"pi_dma\":%llu,"
      "\"overlay_publications\":%llu,"
      "\"overlay_sequence\":\"%s\","
      "\"phase9_route\":{\"player_control_calls\":%llu,"
      "\"weapon_update_calls\":%llu,\"weapon_fire_held_updates\":%llu,"
      "\"weapon_fire_pressed_updates\":%llu,"
      "\"weapon_dummy_fire_calls\":%llu,\"weapon_fire_calls\":%llu,"
      "\"weapon_shot_calls\":%llu,"
      "\"weapon_hit_calls\":%llu,\"enemy_kill_calls\":%llu,"
      "\"weapon_stat_shots\":%llu,\"weapon_stat_hits\":%llu,"
      "\"weapon_stat_kills\":%llu,"
      "\"collision_calls\":%llu,\"particle_update_calls\":%llu,"
      "\"cutscene_active_calls\":%llu,\"cutscene_camera_calls\":%llu,"
      "\"level_change_calls\":%llu,\"death_restart_calls\":%llu,"
      "\"player_hit_check_calls\":%llu,"
      "\"player_actor\":\"0x%08x\","
      "\"player_yaw\":\"0x%04x\","
      "\"player_position_bits\":\"%08x,%08x,%08x\","
      "\"hints_control_calls\":%llu,\"hints_talk_calls\":%llu,"
      "\"hints_actor\":\"0x%08x\",\"hints_yaw\":\"0x%04x\","
      "\"hints_position_bits\":\"%08x,%08x,%08x\","
      "\"hints_state\":\"0x%08x\","
      "\"hints_state_words\":"
      "\"%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,"
      "%08x,%08x,%08x,%08x\","
      "\"king_bear_control_calls\":%llu,"
      "\"king_bear_callback\":\"0x%08x\","
      "\"king_bear_callback_section\":%u,"
      "\"king_bear_callback_offset\":\"0x%08x\","
      "\"king_bear_actor\":\"0x%08x\","
      "\"king_bear_yaw\":\"0x%04x\","
      "\"king_bear_position_bits\":\"%08x,%08x,%08x\","
      "\"king_bear_state\":\"0x%08x\","
      "\"king_bear_state_words\":\"%08x,%08x,%08x,%08x\","
      "\"hinttext_start_calls\":%llu,\"hinttext_stop_calls\":%llu,"
      "\"hinttext_active_calls\":%llu,"
      "\"hinttext_active_zero_returns\":%llu,"
      "\"hinttext_active_nonzero_returns\":%llu,"
      "\"hinttext_active_last_return\":%u,"
      "\"hinttext_update_calls\":%llu,"
      "\"hinttext_advance_calls\":%llu,"
      "\"hinttext_accept_calls\":%llu,"
      "\"hinttext_section_base\":\"0x%08x\","
      "\"hinttext_state_words\":"
      "\"%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,"
      "%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\","
      "\"runlink_module7_words\":"
      "\"%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\"},"
      "\"controller_events\":%llu,"
      "\"generated_calls\":%llu,\"mapped_calls\":%llu,"
      "\"queue_create_calls\":%llu,\"queue_send_calls\":%llu,"
      "\"queue_recv_calls\":%llu,\"sp_task_load_calls\":%llu,"
      "\"sp_task_start_calls\":%llu,"
      "\"controller_read_start_calls\":%llu,"
      "\"controller_get_data_calls\":%llu,\"vi_swap_calls\":%llu,"
      "\"graphics_tasks\":%llu,\"graphics_commands\":%llu,"
      "\"graphics_triangles\":%llu,\"presented_frames\":%llu,"
      "\"decoded_audio_tasks\":%llu,"
      "\"audio_device_initialized\":%s,\"audio_device_started\":%s,"
      "\"audio_device_frequency\":%u,\"audio_device_buffers\":%llu,"
      "\"audio_device_queued_bytes\":%llu,"
      "\"audio_device_consumed_bytes\":%llu,"
      "\"audio_device_consumed_ms\":%llu,"
      "\"audio_device_underruns\":%llu,\"audio_device_overruns\":%llu,"
      "\"ai_submission_overflows\":%llu,\"frame_captured\":%s,"
      "\"input_replay_events\":%zu,\"controller_samples\":%llu,"
      "\"non_neutral_controller_samples\":%llu,"
      "\"controller_disconnects\":%llu,\"controller_reconnects\":%llu,"
      "\"game_pressed_observations\":%llu,"
      "\"game_stick_observations\":%llu,"
      "\"last_game_stick_x\":%d,\"last_game_stick_y\":%d,"
      "\"menu_stick_observations\":%llu,"
      "\"last_menu_stick_x\":%d,\"last_menu_stick_y\":%d,"
      "\"front_mode\":%u,"
      "\"flash_path_configured\":%s,\"flash_image_loaded\":%s,"
      "\"flash_image_created\":%s,\"flash_persist_count\":%llu,"
      "\"threads_stopped\":%zu,\"threads_runnable\":%zu,"
      "\"threads_blocked\":%zu,\"threads_finished\":%zu,"
      "\"hle_counts\":\"%s\","
      "\"thread_summary\":\"%s\",\"controller_read_thread\":%d,"
      "\"si_queue\":\"0x%08x\",\"si_queue_valid\":%u,"
      "\"si_receive_calls\":%llu,\"last_si_receive_return\":\"0x%08x\","
      "\"last_si_receive_block\":%u,"
      "\"last_si_dispatch_trace\":\"%s\","
      "\"journal_events\":%llu,\"journal_entries\":%zu,"
      "\"journal_overflow\":%s,"
      "\"state_hash\":\"%s\",\"journal_hash\":\"%s\"}\n",
      state.poll_target != 0U ? "poll-target" : "retrace-target",
      state.poll_target != 0U ? "poll_target" : "retrace_target",
      state.poll_target != 0U ? state.poll_target : state.retrace_target,
      static_cast<unsigned long long>(state.vi_retraces),
      static_cast<unsigned long long>(state.vi_frames),
      static_cast<unsigned long long>(state.vi_interrupts),
      static_cast<unsigned long long>(state.vi_messages_delivered),
      static_cast<unsigned long long>(state.mmio_trace->accesses),
      static_cast<unsigned long long>(state.mmio_trace->unsupported_accesses),
      static_cast<std::size_t>(state.dispatch_calls),
      static_cast<unsigned long long>(
          event_counts[static_cast<std::size_t>(NativeEventKind::kPiDma)]),
      static_cast<unsigned long long>(event_counts[static_cast<std::size_t>(
          NativeEventKind::kOverlayPublished)]),
      overlay_sequence.c_str(),
      static_cast<unsigned long long>(state.phase9_player_control_calls),
      static_cast<unsigned long long>(state.phase9_weapon_update_calls),
      static_cast<unsigned long long>(state.phase9_weapon_fire_held_updates),
      static_cast<unsigned long long>(state.phase9_weapon_fire_pressed_updates),
      static_cast<unsigned long long>(state.phase9_weapon_dummy_fire_calls),
      static_cast<unsigned long long>(state.phase9_weapon_fire_calls),
      static_cast<unsigned long long>(state.phase9_weapon_shot_calls),
      static_cast<unsigned long long>(state.phase9_weapon_hit_calls),
      static_cast<unsigned long long>(state.phase9_enemy_kill_calls),
      static_cast<unsigned long long>(weapon_stat_shots),
      static_cast<unsigned long long>(weapon_stat_hits),
      static_cast<unsigned long long>(weapon_stat_kills),
      static_cast<unsigned long long>(state.phase9_collision_calls),
      static_cast<unsigned long long>(state.phase9_particle_update_calls),
      static_cast<unsigned long long>(state.phase9_cutscene_active_calls),
      static_cast<unsigned long long>(state.phase9_cutscene_camera_calls),
      static_cast<unsigned long long>(state.phase9_level_change_calls),
      static_cast<unsigned long long>(state.phase9_death_restart_calls),
      static_cast<unsigned long long>(state.phase9_player_hit_check_calls),
      state.phase9_player_actor, static_cast<unsigned>(player_yaw),
      player_position_x, player_position_y, player_position_z,
      static_cast<unsigned long long>(state.phase9_hints_control_calls),
      static_cast<unsigned long long>(state.phase9_hints_talk_calls),
      state.phase9_hints_actor, static_cast<unsigned>(hints_yaw),
      hints_position_x, hints_position_y, hints_position_z,
      hints_state, hints_state_words[0], hints_state_words[1],
      hints_state_words[2], hints_state_words[3], hints_state_words[4],
      hints_state_words[5], hints_state_words[6], hints_state_words[7],
      hints_state_words[8], hints_state_words[9], hints_state_words[10],
      hints_state_words[11],
      static_cast<unsigned long long>(state.phase9_king_bear_control_calls),
      state.phase9_king_bear_callback, king_bear_callback_section,
      king_bear_callback_offset,
      king_bear_actor, static_cast<unsigned>(king_bear_yaw),
      king_bear_position_x, king_bear_position_y, king_bear_position_z,
      king_bear_state, king_bear_state_word_0, king_bear_sound_handle,
      king_bear_state_word_30, king_bear_state_word_34,
      static_cast<unsigned long long>(state.phase9_hinttext_start_calls),
      static_cast<unsigned long long>(state.phase9_hinttext_stop_calls),
      static_cast<unsigned long long>(state.phase9_hinttext_active_calls),
      static_cast<unsigned long long>(
          state.phase9_hinttext_active_zero_returns),
      static_cast<unsigned long long>(
          state.phase9_hinttext_active_nonzero_returns),
      state.phase9_hinttext_active_last_return,
      static_cast<unsigned long long>(state.phase9_hinttext_update_calls),
      static_cast<unsigned long long>(state.phase9_hinttext_advance_calls),
      static_cast<unsigned long long>(state.phase9_hinttext_accept_calls),
      hinttext_section_base,
      hinttext_state_words[0], hinttext_state_words[1],
      hinttext_state_words[2], hinttext_state_words[3],
      hinttext_state_words[4], hinttext_state_words[5],
      hinttext_state_words[6], hinttext_state_words[7],
      hinttext_state_words[8], hinttext_state_words[9],
      hinttext_state_words[10], hinttext_state_words[11],
      hinttext_state_words[12], hinttext_state_words[13],
      hinttext_state_words[14], hinttext_state_words[15],
      runlink_module7_words[0], runlink_module7_words[1],
      runlink_module7_words[2], runlink_module7_words[3],
      runlink_module7_words[4], runlink_module7_words[5],
      runlink_module7_words[6], runlink_module7_words[7],
      static_cast<unsigned long long>(event_counts[static_cast<std::size_t>(
          NativeEventKind::kController)]),
      static_cast<unsigned long long>(state.generated_calls),
      static_cast<unsigned long long>(state.mapped_calls),
      static_cast<unsigned long long>(state.queue_create_calls),
      static_cast<unsigned long long>(state.queue_send_calls),
      static_cast<unsigned long long>(state.queue_recv_calls),
      static_cast<unsigned long long>(state.sp_task_load_calls),
      static_cast<unsigned long long>(state.sp_task_start_calls),
      static_cast<unsigned long long>(state.controller_read_start_calls),
      static_cast<unsigned long long>(state.controller_get_data_calls),
      static_cast<unsigned long long>(state.vi_swap_calls),
      static_cast<unsigned long long>(state.graphics_tasks),
      static_cast<unsigned long long>(state.graphics_commands),
      static_cast<unsigned long long>(state.graphics_triangles),
      static_cast<unsigned long long>(state.presented_frames),
      static_cast<unsigned long long>(state.decoded_audio_tasks),
      state.audio_device_initialized ? "true" : "false",
      state.audio_device_started ? "true" : "false",
      state.audio_device_frequency,
      static_cast<unsigned long long>(state.audio_device_buffers),
      static_cast<unsigned long long>(state.audio_device_queued_bytes),
      static_cast<unsigned long long>(state.audio_device_consumed_bytes),
      static_cast<unsigned long long>(state.audio_device_consumed_ms),
      static_cast<unsigned long long>(state.audio_device_underruns),
      static_cast<unsigned long long>(state.audio_device_overruns),
      static_cast<unsigned long long>(
          state.mmio_trace == nullptr
              ? 0U
              : state.mmio_trace->ai_submission_overflows),
      state.frame_captured ? "true" : "false",
      state.input_replay_event_count,
      static_cast<unsigned long long>(state.controller_samples),
      static_cast<unsigned long long>(state.non_neutral_controller_samples),
      static_cast<unsigned long long>(state.controller_disconnects),
      static_cast<unsigned long long>(state.controller_reconnects),
      static_cast<unsigned long long>(state.game_pressed_observations),
      static_cast<unsigned long long>(state.game_stick_observations),
      static_cast<int>(state.last_game_stick_x),
      static_cast<int>(state.last_game_stick_y),
      static_cast<unsigned long long>(state.menu_stick_observations),
      static_cast<int>(state.last_menu_stick_x),
      static_cast<int>(state.last_menu_stick_y),
      front_mode,
      state.flash_path.empty() ? "false" : "true",
      state.flash_image_loaded ? "true" : "false",
      state.flash_image_created ? "true" : "false",
      static_cast<unsigned long long>(state.flash_persist_count),
      thread_states[static_cast<std::size_t>(ThreadState::kStopped)],
      thread_states[static_cast<std::size_t>(ThreadState::kRunnable)],
      thread_states[static_cast<std::size_t>(ThreadState::kBlocked)],
      thread_states[static_cast<std::size_t>(ThreadState::kFinished)],
      hle_counts.c_str(),
      thread_summary.c_str(), state.controller_read_thread, si_queue,
      si_queue_valid,
      static_cast<unsigned long long>(state.si_receive_calls),
      state.last_si_receive_return, state.last_si_receive_block,
      si_dispatch_trace.c_str(),
      static_cast<unsigned long long>(state.journal.observed_total),
      state.journal.count, state.journal.overflow ? "true" : "false",
      state_digest.c_str(), journal_digest.c_str());
  std::fflush(stdout);
  state.host_audio.finish_capture(); state.timing_trace.finish(); state.guest_clock_trace.finish(); state.input_record.finish(); state.gameplay_trace.finish(); state.snapshot_writer.finish(); state.movement_trace.finish(); state.jump_trace.finish(); std::_Exit(0);
}
#endif

#if defined(JFG_LIVE_RUNTIME)
[[noreturn]] void play_exit(State &state) {
  jfg::support_event("native=closed");
  if (state.mmio_trace == nullptr ||
      state.mmio_trace->unsupported_accesses != 0U) {
    fail_closed_dispatch(state, "play", "invalid-clean-exit", 0U);
  }
  if (!service_host_audio(state, false))
    fail_closed_dispatch(state, "audio", "host-device-final", 0U);
  if (!persist_controller_pak(state))
    fail_closed_dispatch(state, "save", "controller-pak-persist", 0U);
  if (!write_live_frame_capture(state))
    fail_closed_dispatch(state, "renderer", "frame-capture", 0U);
  if (!write_private_rdram_capture(state))
    fail_closed_dispatch(state, "runtime", "rdram-capture", 0U);
  const unsigned front_mode = state.rdram[0x000A51B0U ^ 3U];
  std::fprintf(
      stdout,
      "{\"kind\":\"jfg-phase8-play\",\"status\":\"window-closed\","
      "\"vi_retraces\":%llu,\"graphics_tasks\":%llu,"
      "\"presented_frames\":%llu,\"decoded_audio_tasks\":%llu,"
      "\"audio_device_initialized\":%s,\"audio_device_started\":%s,"
      "\"audio_device_frequency\":%u,\"audio_device_buffers\":%llu,"
      "\"audio_device_consumed_bytes\":%llu,"
      "\"audio_device_consumed_ms\":%llu,"
      "\"audio_device_underruns\":%llu,\"audio_device_overruns\":%llu,"
      "\"frame_captured\":%s,"
      "\"controller_samples\":%llu,"
      "\"non_neutral_controller_samples\":%llu,"
      "\"non_neutral_controller_writes\":%llu,"
      "\"controller_disconnects\":%llu,\"controller_reconnects\":%llu,"
      "\"game_pressed_observations\":%llu,"
      "\"last_game_pressed_buttons\":\"0x%04x\","
      "\"game_stick_observations\":%llu,"
      "\"last_game_stick_x\":%d,\"last_game_stick_y\":%d,"
      "\"menu_stick_observations\":%llu,"
      "\"last_menu_stick_x\":%d,\"last_menu_stick_y\":%d,"
      "\"front_mode\":%u,"
      "\"unsupported_accesses\":0,\"journal_overflow\":%s}\n",
      static_cast<unsigned long long>(state.vi_retraces),
      static_cast<unsigned long long>(state.graphics_tasks),
      static_cast<unsigned long long>(state.presented_frames),
      static_cast<unsigned long long>(state.decoded_audio_tasks),
      state.audio_device_initialized ? "true" : "false",
      state.audio_device_started ? "true" : "false",
      state.audio_device_frequency,
      static_cast<unsigned long long>(state.audio_device_buffers),
      static_cast<unsigned long long>(state.audio_device_consumed_bytes),
      static_cast<unsigned long long>(state.audio_device_consumed_ms),
      static_cast<unsigned long long>(state.audio_device_underruns),
      static_cast<unsigned long long>(state.audio_device_overruns),
      state.frame_captured ? "true" : "false",
      static_cast<unsigned long long>(state.controller_samples),
      static_cast<unsigned long long>(state.non_neutral_controller_samples),
      static_cast<unsigned long long>(state.non_neutral_controller_writes),
      static_cast<unsigned long long>(state.controller_disconnects),
      static_cast<unsigned long long>(state.controller_reconnects),
      static_cast<unsigned long long>(state.game_pressed_observations),
      state.last_game_pressed_buttons,
      static_cast<unsigned long long>(state.game_stick_observations),
      static_cast<int>(state.last_game_stick_x),
      static_cast<int>(state.last_game_stick_y),
      static_cast<unsigned long long>(state.menu_stick_observations),
      static_cast<int>(state.last_menu_stick_x),
      static_cast<int>(state.last_menu_stick_y),
      front_mode,
      state.journal.overflow ? "true" : "false");
  std::fflush(stdout);
  state.host_audio.finish_capture(); state.timing_trace.finish(); state.guest_clock_trace.finish(); state.input_record.finish(); state.gameplay_trace.finish(); state.snapshot_writer.finish(); state.movement_trace.finish(); state.jump_trace.finish(); std::_Exit(0);
}
#endif

[[noreturn]] void retrace_gate(State &state) {
  if (state.poll_target != 0U)
    fail_closed_dispatch(state, "probe", "poll-not-reached", 0U);
  if (state.retrace_target == 3U)
    stable_vi_gate(state);
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  probe_target_gate(state);
#else
  fail_closed_dispatch(state, "native", "unexpected-retrace-target", 0U);
#endif
}

#if defined(JFG_LIVE_RUNTIME)
void complete_guest_sp_status(State &state, const std::uint32_t target) {
  if (!state.guest_leaf_probe) return;
  if (state.mmio_trace == nullptr || !state.mmio_trace->guest_sp_status ||
      !state.mmio_trace->sp_status.complete_task())
    fail_closed_dispatch(state, "rsp", "completion-status", target);
  // The current HLE interrupt service acknowledges before posting its OS
  // message. It still has no guest interrupt-handler execution/timing.
  state.mmio_trace->sp_status.acknowledge_interrupt();
}

// RGO host speed: hand a graphics snapshot to the renderer. Its RAM copy
// holds the last snapshot it imported (plus present windows since); when
// this snapshot was built on top of that one, only the 4 KB chunks that
// differ are copied, instead of comparing all 8 MB. The lower 4 MB changes
// were found while copying guest RAM into the snapshot (and display-list
// patches); the overlay half is compared against the copy of it imported
// last, over the overlay shadows only.
jfg::Rt64ShellError import_graphics_snapshot(State &state, PendingLiveGraphicsTask &pending) {
  constexpr std::size_t kChunk = jfg::kSnapshotChunkBytes;
  constexpr std::size_t kUpper = jfg::kRt64RequiredRdramBytes - kRdramSize;
  const bool delta = !state.snapshot_full_import && pending.base_id != 0U &&
      pending.base_id == state.imported_snapshot_id && state.imported_upper.size() == kUpper &&
      pending.rdram.size() == jfg::kRt64RequiredRdramBytes;
  jfg::Rt64ShellError error = jfg::Rt64ShellError::invalid_memory;
  if (delta) {
    for (const LiveGraphicsOverlayShadow &shadow : state.graphics_overlay_shadows) {
      if (shadow.physical_base < kRdramSize || shadow.extent == 0U ||
          shadow.physical_base > jfg::kRt64RequiredRdramBytes ||
          shadow.extent > jfg::kRt64RequiredRdramBytes - shadow.physical_base)
        continue;
      for (std::size_t offset = shadow.physical_base / kChunk * kChunk;
           offset < shadow.physical_base + shadow.extent; offset += kChunk) {
        std::byte *const mirror = state.imported_upper.data() + (offset - kRdramSize);
        if (std::memcmp(pending.rdram.data() + offset, mirror, kChunk) != 0) {
          std::memcpy(mirror, pending.rdram.data() + offset, kChunk);
          pending.changes.mark_chunk(offset / kChunk);
        }
      }
    }
    for (const std::uint32_t chunk : state.import_extra_chunks)
      pending.changes.mark_chunk(chunk);
    error = state.rt64_shell->import_rdram_chunks(pending.rdram, pending.changes.chunks(),
                                                  jfg::Rt64MemoryLayout::host_word_swapped);
    if (error == jfg::Rt64ShellError::none) {
      ++state.snapshot_delta_imports;
      state.snapshot_delta_chunks += pending.changes.chunks().size();
      if (state.snapshot_verify)
        for (const std::uint32_t chunk : pending.changes.chunks())
          std::memcpy(state.snapshot_verify_model.data() + std::size_t{chunk} * kChunk,
                      pending.rdram.data() + std::size_t{chunk} * kChunk, kChunk);
    }
  }
  if (error == jfg::Rt64ShellError::invalid_memory) {
    error = state.rt64_shell->replace_rdram_snapshot(
        pending.rdram, jfg::Rt64MemoryLayout::host_word_swapped);
    if (error == jfg::Rt64ShellError::none) {
      ++state.snapshot_full_imports;
      state.imported_upper.assign(pending.rdram.begin() + kRdramSize, pending.rdram.end());
      if (state.snapshot_verify)
        state.snapshot_verify_model = pending.rdram;
    }
  }
  state.import_extra_chunks.clear();
  state.imported_snapshot_id = error == jfg::Rt64ShellError::none ? pending.snapshot_id : 0U;
  // JFG_RGO_SNAPSHOT_VERIFY=1: the model of the renderer's copy must equal
  // the snapshot after every import.
  if (state.snapshot_verify && error == jfg::Rt64ShellError::none &&
      state.snapshot_verify_model.size() == pending.rdram.size()) {
    std::uint64_t mismatches = 0U;
    std::size_t first = SIZE_MAX;
    for (std::size_t offset = 0U; offset < pending.rdram.size(); offset += kChunk)
      if (std::memcmp(state.snapshot_verify_model.data() + offset,
                      pending.rdram.data() + offset, kChunk) != 0) {
        ++mismatches;
        first = (std::min)(first, offset);
      }
    if (mismatches != 0U) {
      if (state.snapshot_verify_mismatches == 0U)
        std::fprintf(stderr, "snapshot verify: %llu chunk(s) differ, first at 0x%zx (snapshot %llu)\n",
                     static_cast<unsigned long long>(mismatches), first,
                     static_cast<unsigned long long>(pending.snapshot_id));
      state.snapshot_verify_mismatches += mismatches;
      state.snapshot_verify_model = pending.rdram;  // resynchronise the model
    }
  }
  return error;
}

void complete_pending_live_graphics_tasks(
    State &state, const std::uint32_t target,
    const bool require_offscreen_target = false) {
  if (state.pending_graphics_tasks.empty())
    return;
  const auto sp = state.events.find(4U);
  const auto dp = state.events.find(9U);
  if (state.rt64_shell == nullptr || (!state.guest_os_probe && (state.scheduler == nullptr ||
      sp == state.events.end() || dp == state.events.end())))
    fail_closed_dispatch(state, "renderer", "completion-events", target);
  const std::uint32_t selected_vi_framebuffer =
      (state.vi_next_framebuffer != 0U ? state.vi_next_framebuffer
                                      : state.vi_current_framebuffer) &
      0x00FFFFFFU;
  for (PendingLiveGraphicsTask &pending : state.pending_graphics_tasks) {
    if (pending.skipped) {
      if (state.guest_os_probe)
        fail_closed_dispatch(state, "renderer", "guest-task-skipped", target);
      ++state.graphics_tasks;
      complete_guest_sp_status(state, target);
      if (state.scheduler->send(sp->second.first, sp->second.second, false) !=
          hle::kOsSuccess)
        fail_closed_dispatch(state, "rsp", "sp-completion", target);
      const std::int32_t dp_delivery = state.scheduler->send(
          dp->second.first, dp->second.second, false);
      if (dp_delivery != hle::kOsSuccess && dp_delivery != hle::kOsFull)
        fail_closed_dispatch(state, "rsp", "dp-completion", target);
      continue;
    }
    if (!state.rdram_capture_path.empty() &&
        state.vi_retraces + 1U >= state.retrace_target &&
        !write_private_rt64_snapshot(state, pending.rdram))
      fail_closed_dispatch(state, "renderer", "rt64-rdram-capture", target);
    write_private_progress(state, "graphics-submit-begin");
    const auto submit_start = std::chrono::steady_clock::now();
    state.last_rt64_error = import_graphics_snapshot(state, pending);
    if (state.last_rt64_error == jfg::Rt64ShellError::none)
      state.last_rt64_error = state.rt64_shell->submit(pending.task);
    // Return completed intensity masks before delivering DP/SP completion:
    // the guest blurs and attenuates its shadow mask on the CPU.
    if (state.last_rt64_error == jfg::Rt64ShellError::none)
      state.last_rt64_error = state.rt64_shell->commit_cpu_writeback(pending.rdram,
          std::as_writable_bytes(std::span(state.rdram, kRdramSize)));
    const jfg::Rt64GraphicsDiagnostics submission_graphics =
        state.rt64_shell->last_graphics_diagnostics();
    if (state.last_rt64_error == jfg::Rt64ShellError::none)
      state.host_audio.renderer_ready();

    const auto submit_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - submit_start)
            .count());
    state.gameplay_trace.event("submit", {state.vi_retraces, state.controller_samples,
        state.gameplay_trace_updates, submit_us, pending.descriptor[4], pending.descriptor[5],
        submission_graphics.rdram_check_microseconds, submission_graphics.full_sync_microseconds,
        submission_graphics.color_image_address});
    state.graphics_submit_microseconds += submit_us;
    state.graphics_submit_max_us =
        (std::max)(state.graphics_submit_max_us, submit_us);
    if (state.timing_trace) {
      state.timing_trace << "submit\t" << state.vi_frames << '\t'
                         << state.vi_retraces << '\t' << submit_us << '\t'
                         << std::hex << pending.descriptor[4] << '\t'
                         << pending.descriptor[5] << std::dec << '\t'
                         << submission_graphics.rdram_check_microseconds
                         << '\t'
                         << submission_graphics.full_sync_microseconds << '\t'
                         << std::hex << submission_graphics.color_image_address
                         << std::dec << '\n';
    }
    write_private_progress(state, "graphics-submit-end");
    if (state.last_rt64_error != jfg::Rt64ShellError::none) {
      const jfg::Rt64GraphicsDiagnostics rejected = submission_graphics;
      if (!state.rdram_capture_path.empty())
        (void)write_private_rt64_snapshot(state, pending.rdram);
      std::array<char, 192U> operation{};
      (void)std::snprintf(
          operation.data(), operation.size(),
          "submit-%u-c%08x-n%x-r%08x-w%08x-%08x-q%u-d%08x-"
          "s%08x-t%u-l%016llx-%04x",
          static_cast<unsigned>(state.last_rt64_error), pending.descriptor[4],
          pending.descriptor[5], rejected.rejected_command_address,
          rejected.rejected_command_word0, rejected.rejected_command_word1,
          rejected.rejection_reason, rejected.rejection_detail,
          rejected.rejection_source_address, rejected.rejection_triangle,
          static_cast<unsigned long long>(rejected.loaded_vertices_low),
          static_cast<unsigned>(rejected.loaded_vertices_high));
      fail_closed_dispatch(state, "renderer", operation.data(), target);
    }
    const jfg::Rt64GraphicsDiagnostics graphics = submission_graphics;
    const std::uint32_t current_target =
        state.vi_current_framebuffer & 0x00FFFFFFU;
    const std::uint32_t next_target =
        state.vi_next_framebuffer & 0x00FFFFFFU;
    if (require_offscreen_target &&
        (graphics.color_image_address == 0U ||
         graphics.color_image_address == current_target ||
         graphics.color_image_address == next_target))
      fail_closed_dispatch(state, "renderer", "auxiliary-target", target);
    ++state.graphics_tasks;
    state.graphics_commands += state.rt64_shell->last_command_count();
    state.graphics_triangles += graphics.triangles_drawn;
    const std::size_t graphics_slot =
        state.recent_graphics_position %
        state.recent_graphics_color_images.size();
    state.recent_graphics_command_addresses[graphics_slot] =
        pending.descriptor[4];
    state.recent_graphics_color_images[graphics_slot] =
        graphics.color_image_address;
    state.recent_graphics_vi_selections[graphics_slot] =
        selected_vi_framebuffer;
    state.recent_graphics_matrix_commands[graphics_slot] =
        static_cast<std::uint32_t>(graphics.matrix_commands);
    state.recent_graphics_vertices_loaded[graphics_slot] =
        static_cast<std::uint32_t>(graphics.vertices_loaded);
    state.recent_graphics_vertices_in_clip[graphics_slot] =
        static_cast<std::uint32_t>(graphics.vertices_in_clip);
    state.recent_graphics_triangles[graphics_slot] =
        static_cast<std::uint32_t>(graphics.triangles_drawn);
    ++state.recent_graphics_position;
    if (graphics.color_image_address != 0U) {
      state.graphics_color_image = graphics.color_image_address;
      state.graphics_frame_ready = true;
      state.frame_content.drawn(graphics.color_image_address & 0x00FFFFFFU,
                                graphics.scene_hash, graphics.camera_hash);

    }
    // Submission and writeback have consumed this snapshot. RT64 owns the
    // resident render targets; keeping a snapshot is not proof of residency.
    state.graphics_snapshot_pool.push_back({std::move(pending.rdram), pending.snapshot_id});
    if (state.guest_os_probe) continue; // device event owns status/IRQ; original OS owns messages
    complete_guest_sp_status(state, target);
    if (state.scheduler->send(sp->second.first, sp->second.second, false) !=
        hle::kOsSuccess)
      fail_closed_dispatch(state, "rsp", "sp-completion", target);
    const std::int32_t dp_delivery =
        state.scheduler->send(dp->second.first, dp->second.second, false);
    if (dp_delivery != hle::kOsSuccess && dp_delivery != hle::kOsFull)
      fail_closed_dispatch(state, "rsp", "dp-completion", target);
  }
  state.pending_graphics_tasks.clear();
}
#endif

void set_result(recomp_context *context, std::int32_t result) {
  context->r2 = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
}

#if defined(JFG_PHASE8_LIVE_RUNTIME)
bool complete_flash_operation(State &state) {
  const jfg::FlashRamResult result =
      state.flash_path.empty()
          ? state.flashram.complete()
          : state.flashram.complete_and_persist(state.flash_path);
  if (result.ok() && !state.flash_path.empty())
    ++state.flash_persist_count;
  return result.ok();
}
#endif

bool guest_byte_offset(const std::uint32_t address, std::size_t &offset) {
  const std::uint32_t segment = address & 0xE0000000U;
  const std::uint32_t physical = address & 0x1FFFFFFFU;
  if ((segment != 0x80000000U && segment != 0xA0000000U) ||
      physical >= kRdramSize)
    return false;
  offset = static_cast<std::size_t>(physical) ^ 3U;
  return true;
}
bool write_guest_byte(State &state, const std::uint32_t address,
                      const std::uint8_t value) {
  std::size_t offset = 0U;
  if (!guest_byte_offset(address, offset))
    return false;
  state.rdram[offset] = value;
  return true;
}
// Every path that publishes a synthetic overlay reports a failure the same
// way: the publisher's recorded reason, under the generated category.
[[noreturn]] void fail_closed_overlay_publication(State &state,
                                                 const std::uint32_t target) {
  std::array<char, 96U> operation{};
  (void)std::snprintf(operation.data(), operation.size(),
                      "overlay-publication-%s",
                      state.last_overlay_publication_failure);
  fail_closed_dispatch(state, "generated", operation.data(), target);
}

// One VI frame of host service while the scheduler is quiescent: frame
// budget, journal, window/audio service, presentation of the completed
// workload, pending graphics-task completion, VI field selection, SI and VI
// message delivery. Extracted from the osStartThread handler so the
// scheduler loop reads as a loop; behavior is unchanged.
// Read-only SI timing evidence. Result is a queue return code, not a claim
// about hardware interrupt latency. Enabled only by focused update capture.
void trace_si_timing(State &state, const char *event, std::uint32_t queue,
                     std::uint32_t caller, std::int32_t result) {
#if defined(JFG_LIVE_RUNTIME)
  const auto si = state.events.find(5U);
  if (si == state.events.end() || queue != si->second.first ||
      state.focus_update_start == 0U || !state.timing_trace.is_open() ||
      state.completed_game_updates + 1U < state.focus_update_start ||
      state.completed_game_updates + 1U > state.focus_update_end + 1U) return;
  state.timing_trace << "si-timing\t" << state.vi_frames << '\t'
      << state.vi_retraces << '\t' << state.controller_samples << '\t'
      << state.completed_game_updates << '\t' << event << '\t'
      << std::hex << queue << '\t' << caller << std::dec << '\t'
      << result << '\t' << state.cpu_count << '\n';
#else
  (void)state; (void)event; (void)queue; (void)caller; (void)result;
#endif
}

void service_si_deadline(State &state, const std::uint32_t target) {
  const auto completion = state.si_deadline.take(state.cpu_count);
  if (!completion) return;
  if (state.scheduler == nullptr || state.pending_si_completions != 1U)
    fail_closed_dispatch(state, "si", "deadline-ownership", target);
  state.pending_si_completions = 0U;
  const auto result = state.scheduler->send(completion->queue, completion->message, false);
  trace_si_timing(state, "deadline-deliver", completion->queue, 0U, result);
  state.journal.append(NativeEventKind::kController, 4U, completion->queue,
                       result == hle::kOsSuccess ? 1U : 0U);
  if (result == hle::kOsSuccess && state.scheduler_running &&
      state.scheduler->current_thread_id() >= 0)
    state.scheduler->yield_current();
}

void schedule_si_deadline(State &state, const std::uint32_t target) {
  const auto event = state.events.find(5U);
  // Measured oracle DMA-return-to-ack interval for the pinned early case.
  // This is an opt-in causal experiment, NOT an N64 hardware latency claim.
  constexpr std::uint64_t observed_ack_ticks = 2568U;
  if (event == state.events.end() || state.pending_si_completions != 1U ||
      !state.si_deadline.start(state.cpu_count, observed_ack_ticks,
                               event->second.first, event->second.second))
    fail_closed_dispatch(state, "si", "deadline-submission", target);
}

void service_timers(State &state, const std::uint32_t target) {
  if (!state.timers.next().has_value()) return;
  if (state.scheduler == nullptr)
    fail_closed_dispatch(state, "timer", "missing-scheduler", target);
  hle::GuestMemory memory({state.rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  bool delivered = false;
  if (!state.timers.service(memory, state.cpu_count,
      [&](std::uint32_t queue, std::uint32_t message) {
        delivered |= state.scheduler->send(queue, message, false) == hle::kOsSuccess;
      }))
    fail_closed_dispatch(state, "timer", "invalid-timer-state", target);
  if (delivered && state.scheduler_running &&
      state.scheduler->current_thread_id() >= 0)
    state.scheduler->yield_current();
}

// Diagnostic only: the dispatch-based Count is not hardware CPU timing.
void trace_guest_clock(State &state, const char *event, std::uint32_t address) {
#if defined(JFG_LIVE_RUNTIME)
  if (state.focus_update_start == 0U || !state.timing_trace.is_open() ||
      state.completed_game_updates + 1U < state.focus_update_start - 1U ||
      state.completed_game_updates + 1U > state.focus_update_end + 1U) return;
  state.timing_trace << "guest-clock\t" << state.completed_game_updates << '\t'
      << state.controller_samples << '\t' << state.vi_retraces << '\t'
      << event << '\t' << std::hex << address << std::dec << '\t'
      << state.cpu_count << '\n';
  state.timing_trace << "execution-work\t" << state.completed_game_updates << '\t'
      << state.controller_samples << '\t' << state.vi_retraces << '\t'
      << event << '\t' << std::hex << address << std::dec << '\t'
      << state.observed_guest_instructions << '\n';
  if (state.observed_guest_instructions != 0U) {
    state.timing_trace << "execution-hle\t" << state.completed_game_updates << '\t'
        << event << '\t' << std::hex << address << std::dec;
    for (std::size_t index = 0U; index < kMappingCount; ++index) {
      if (state.mapped_call_counts[index] != 0U)
        state.timing_trace << '\t' << kMappings[index].name << '='
            << state.mapped_call_counts[index];
    }
    state.timing_trace << '\n';
    state.timing_trace << "execution-dispatches\t" << state.completed_game_updates
        << '\t' << event << '\t' << std::hex << address;
    const auto count = (std::min)(state.recent_dispatch_position, state.recent_dispatches.size());
    for (std::size_t offset = 0U; offset < count; ++offset)
      state.timing_trace << '\t' << state.recent_dispatches[
          (state.recent_dispatch_position - count + offset) % state.recent_dispatches.size()];
    state.timing_trace << std::dec << '\n';
  }
#else
  (void)state; (void)event; (void)address;
#endif
}

// Legacy cooperative clock only. Keep this out of the device/presentation
// operations below so an execution-driven dispatcher can call those at
// independent deadlines without adding a frame of time a second time.
void advance_legacy_vi_clock(State &state, const std::uint32_t target) {
  trace_guest_clock(state, "vi-service-entry", target);
  const std::uint64_t frame_budget =
      static_cast<std::uint64_t>(state.retrace_target) * 20U;
  if (++state.vi_frames > frame_budget) {
    fail_closed_dispatch(state, "vi", "frame-budget", target);
  }
  const auto frame_end = state.cpu_count + kCountTicksPerViFrame;
  // A blocked receiver can resume at a timer deadline between VI events.
  // Do not round the controller power-on wait to whole video frames.
  while (true) {
    auto deadline = state.timers.next();
    const auto si_deadline = state.si_deadline.next();
    if (si_deadline && (!deadline || *si_deadline < *deadline)) deadline = si_deadline;
    if (!deadline) break;
    if (*deadline > frame_end) break;
    state.cpu_count = (std::max)(state.cpu_count, *deadline);
    service_si_deadline(state, target);
    service_timers(state, target);
    const auto outcome = state.scheduler->run(1'000'000U);
    if (outcome == ScheduleOutcome::kUnbounded)
      fail_closed_dispatch(state, "timer", "scheduler-step-budget", target);
    if (state.cpu_count >= frame_end) break;
  }
  state.cpu_count = (std::max)(state.cpu_count, frame_end);
  trace_guest_clock(state, "vi-deadline", target);
}

// Presentation consumes already completed work. It does not complete a
// graphics task, advance Count, or deliver a guest interrupt.
void present_completed_video(State &state, const std::uint32_t target) {
#if defined(JFG_LIVE_RUNTIME)
  if (state.play_mode || state.realtime_replay) {
    service_live_window(state);
    if (state.exit_requested)
      play_exit(state);
  }
  if (!service_host_audio(state, true))
    fail_closed_dispatch(state, "audio", "host-device", target);
  const std::uint32_t selected_vi_framebuffer =
      (state.vi_next_framebuffer != 0U
           ? state.vi_next_framebuffer
           : state.vi_current_framebuffer) &
      0x00FFFFFFU;
  // The task completed during this scheduler slice targets JFG's
  // off-screen buffer. Present the previously completed workload before
  // submitting that new task, so the normal double-buffer pipeline needs
  // one RT64 submission per displayed frame instead of two.
  if (state.rt64_shell != nullptr && state.graphics_frame_ready &&
      selected_vi_framebuffer != 0U) {
    // Effects can submit more than eight off-screen targets between VI
    // presentations. Ask RT64 whether the selected color target exists instead
    // of letting an unrelated snapshot-cache eviction suppress the frame.
    const bool presentation_ready =
        state.rt64_shell->has_color_framebuffer(selected_vi_framebuffer);
    if (!presentation_ready) {
      state.gameplay_trace.event("present-skip", {state.vi_retraces,
          state.controller_samples, state.gameplay_trace_updates,
          selected_vi_framebuffer, state.graphics_color_image});
    }
    if (presentation_ready) {
      state.rt64_vi.current_line =
          state.rt64_vi.current_line == 0U ? 2U : 0U;
      if (!apply_live_vi_field(state, selected_vi_framebuffer))
        fail_closed_dispatch(state, "renderer", "vi-field", target);
      // CPU overlays can modify the completed framebuffer between DP and VI.
      // Refresh the displayed pixels before RT64 tests its framebuffer hash.
      // RGO host speed: every task import already brought the renderer's RAM
      // copy up to date, so only the displayed framebuffer can be stale here;
      // import that window (up to 640x480 at 32 bits) instead of copying and
      // comparing all of RAM (JFG_RGO_PRESENT_FULL_IMPORT=1: the full import).
      const auto [window_begin, window_end] = jfg::present_import_window(
          selected_vi_framebuffer, static_cast<std::uint32_t>(kRdramSize));
      state.last_rt64_error = state.present_full_import
          ? jfg::Rt64ShellError::invalid_memory
          : state.rt64_shell->import_rdram_range(
                std::as_bytes(std::span(state.rdram, kRdramSize)), window_begin, window_end,
                jfg::Rt64MemoryLayout::host_word_swapped);
      if (state.last_rt64_error == jfg::Rt64ShellError::none) {
        // The renderer's copy now differs from the last imported snapshot in
        // this window: the next task's delta import refreshes it.
        for (auto chunk = static_cast<std::uint32_t>(window_begin / jfg::kSnapshotChunkBytes);
             chunk * jfg::kSnapshotChunkBytes < window_end; ++chunk)
          state.import_extra_chunks.push_back(chunk);
        if (state.snapshot_verify && state.snapshot_verify_model.size() >= window_end)
          std::memcpy(state.snapshot_verify_model.data() + window_begin,
                      state.rdram + window_begin, window_end - window_begin);
      } else {
        // Preserve the upper RDRAM scratch space used by captured display lists.
        std::memcpy(state.rt64_rdram.data(), state.rdram, kRdramSize);
        state.last_rt64_error = state.rt64_shell->replace_rdram_snapshot(
            state.rt64_rdram, jfg::Rt64MemoryLayout::host_word_swapped);
        state.imported_snapshot_id = 0U;  // the copy no longer holds a snapshot
        if (state.snapshot_verify)
          state.snapshot_verify_model = state.rt64_rdram;
      }
      if (state.last_rt64_error != jfg::Rt64ShellError::none)
        fail_closed_dispatch(state, "renderer", "present-cpu-import", target);
      refresh_video_settings(state);
      write_private_progress(state, "graphics-present-begin");
      const auto present_start = std::chrono::steady_clock::now();
      const bool capture_frame = !state.frame_capture_path.empty() &&
          (state.play_mode
               ? (state.presented_frames + 1U) % 60U == 0U
               : state.vi_retraces + 2U >= state.retrace_target);
      state.last_rt64_error =
          state.rt64_shell->present(capture_frame);
      const auto present_end = std::chrono::steady_clock::now();
      const auto present_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              present_end - present_start)
              .count());
      state.graphics_present_microseconds += present_us;
      state.graphics_present_max_us =
          (std::max)(state.graphics_present_max_us, present_us);
      std::uint64_t present_interval_us = 0U;
      if (state.present_time_initialized) {
        present_interval_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                present_end - state.present_time)
                .count());
        state.present_interval_max_us =
            (std::max)(state.present_interval_max_us,
                       present_interval_us);
      }
      state.gameplay_trace.event("present", {state.vi_retraces, state.controller_samples,
          state.gameplay_trace_updates, present_us, present_interval_us, selected_vi_framebuffer});
      state.present_time = present_end;
      state.present_time_initialized = true;
      if (present_interval_us != 0U)
        state.frame_gaps.add(present_interval_us);
      const auto content = state.frame_content.presented(selected_vi_framebuffer);
      if (!content.fresh) ++state.frame_repeats;
      else {
        ++state.frame_drawn;
        if (content.scene_changed) ++state.frame_new;
        if (content.camera_changed) ++state.frame_camera;
      }
      if (state.timing_trace) state.timing_trace << "present-qpc\t"
          << state.vi_frames << '\t' << state.vi_retraces << '\t'
          << std::chrono::duration_cast<std::chrono::microseconds>(
                 present_end.time_since_epoch()).count() << '\n';
      if (state.timing_trace) {
        state.timing_trace << "present\t" << state.vi_frames << '\t'
                           << state.vi_retraces << '\t' << present_us
                           << '\t' << present_interval_us << '\t'
                           << std::hex << selected_vi_framebuffer
                           << std::dec << '\n';
      }
      write_private_progress(state, "graphics-present-end");
      if (state.last_rt64_error != jfg::Rt64ShellError::none) {
        std::array<char, 96U> operation{};
        (void)std::snprintf(
            operation.data(), operation.size(),
            "present-%u-%08x-%08x-%08x",
            static_cast<unsigned>(state.last_rt64_error),
            state.rt64_vi.origin, state.graphics_color_image,
            state.rt64_vi.width);
        fail_closed_dispatch(state, "renderer", operation.data(),
                             target);
      }
      ++state.presented_frames;
      state.graphics_frame_ready = false;
    }
  }
#else
  (void)state; (void)target;
#endif
}

void latch_next_vi_framebuffer(State &state, const std::uint32_t target) {
#if defined(JFG_LIVE_RUNTIME)
  if (state.vi_next_framebuffer != 0U) {
    state.vi_current_framebuffer = state.vi_next_framebuffer;
    if (!apply_live_vi_field(state, state.vi_current_framebuffer))
      fail_closed_dispatch(state, "renderer", "vi-field", target);
  }
#else
  (void)state; (void)target;
#endif
}

void deliver_legacy_si_completion(State &state, const std::uint32_t target) {
  if (!state.si_count_probe && state.pending_si_completions != 0U) {
    const auto si = state.events.find(5U);
    if (si == state.events.end()) {
      fail_closed_dispatch(state, "controller", "si-delivery", target);
    }
    const std::int32_t delivery = state.scheduler->send(
        si->second.first, si->second.second, false);
    if (delivery != hle::kOsSuccess && delivery != hle::kOsFull)
      fail_closed_dispatch(state, "controller", "si-delivery", target);
    --state.pending_si_completions;
    trace_si_timing(state, "deliver", si->second.first, 0U, delivery);
    state.journal.append(NativeEventKind::kController, 4U,
                         si->second.first,
                         delivery == hle::kOsSuccess ? 1U : 0U);
  }
}

void deliver_vi_interrupt(State &state, const std::uint32_t target) {
  if (state.vi_frames % state.vi_retrace_interval == 0U) {
    if (state.scheduler->send(state.vi_internal_queue,
                              state.vi_internal_message, false) !=
        hle::kOsSuccess) {
      fail_closed_dispatch(state, "vi", "interrupt-delivery", target);
    }
    ++state.vi_interrupts;
    state.journal.append(NativeEventKind::kViInterruptRaised,
                         state.vi_internal_queue,
                         state.vi_internal_message,
                         static_cast<std::uint32_t>(
                             state.vi_interrupts));
  }
}

// Preserve the cooperative profile's exact order as a control. Device
// completion is explicitly separate from VI service; this wrapper still
// schedules them together and does NOT claim execution-driven timing.
void service_vi_frame(State &state, const std::uint32_t target) {
  advance_legacy_vi_clock(state, target);
  state.empty_receive_polls.clear();
  state.journal.append(NativeEventKind::kViFrame,
                       static_cast<std::uint32_t>(state.vi_frames));
  present_completed_video(state, target);
#if defined(JFG_LIVE_RUNTIME)
  complete_pending_live_graphics_tasks(state, target);
#endif
  latch_next_vi_framebuffer(state, target);
  deliver_legacy_si_completion(state, target);
  deliver_vi_interrupt(state, target);
}

// Overlay 32's mrhintsControl entry (text offset 0x2E8) doubles as the King
// Bear hint callback. Three dispatch paths reach it (direct, generated
// overlay lookup, runlink-resolved lookup) and must record identical route
// diagnostics.
void note_hints_control_call(State &state, const std::uint32_t target,
                             const recomp_context &context) {
  ++state.phase9_hints_control_calls;
  state.phase9_hints_actor = static_cast<std::uint32_t>(context.r4);
  ++state.phase9_king_bear_control_calls;
  state.phase9_king_bear_actor = static_cast<std::uint32_t>(context.r4);
  state.phase9_king_bear_callback = target;
}

#if defined(JFG_LIVE_RUNTIME)
// RGO widescreen HUD anchoring (jfg/mod/widescreen_hud.hpp).
class HudGroupScope {
public:
  HudGroupScope(jfg::mod::WidescreenHud &hud, const std::uint32_t function)
      : hud_(hud) { hud_.enter(function); }
  ~HudGroupScope() { hud_.exit(); }
  HudGroupScope(const HudGroupScope &) = delete;
  HudGroupScope &operator=(const HudGroupScope &) = delete;
private:
  jfg::mod::WidescreenHud &hud_;
};

bool hud_qualified(const hle::GuestMemory &memory,
                   const jfg::mod::HudFunction &function) {
  std::uint32_t first = 0U, second = 0U;
  return memory.read_u32(function.address, first) &&
         memory.read_u32(function.address + 4U, second) &&
         first == function.prologue[0] && second == function.prologue[1];
}

std::uint32_t hud_framebuffer_width(const State &state) {
  const std::uint32_t width = state.rt64_vi.width & 0xFFFU;
  return width >= 64U && width <= 1024U ? width : 320U;
}

// Follow the HUD display list up to `cursor` and report whether the current
// scissor spans the whole framebuffer width.
bool hud_full_width_scissor(State &state, const hle::GuestMemory &memory,
                            const std::uint32_t cursor) {
  std::uint32_t position = state.hud.scan_cursor();
  bool full = state.hud.scissor_full();
  if (position != 0U && cursor >= position && cursor - position <= 0x40000U) {
    const std::uint32_t width = hud_framebuffer_width(state);
    for (; cursor - position >= 8U; position += 8U) {
      std::uint32_t word0 = 0U, word1 = 0U;
      if (!memory.read_u32(position, word0) ||
          !memory.read_u32(position + 4U, word1))
        return false;
      if ((word0 >> 24U) == 0xEDU)
        full = jfg::mod::hud_scissor_full_width(word0, word1, width);
    }
    state.hud.set_scissor_full(full);
  } else {
    full = false;  // Lost track of the list: leave this element alone.
  }
  state.hud.set_scan_cursor(cursor);
  return full;
}

// Wrap one text or rectangle primitive in RT64 rectangle alignment written
// into its own display list, then reset the alignment after it.
bool hud_aligned_call(State &state, hle::GuestMemory &memory,
                      const std::uint32_t list, const std::uint32_t target,
                      std::uint8_t *rdram, recomp_context *context) {
  const jfg::mod::HudAnchor anchor = state.hud.anchor();
  std::uint32_t cursor = 0U;
  if (anchor == jfg::mod::HudAnchor::none || !memory.read_u32(list, cursor) ||
      !hud_full_width_scissor(state, memory, cursor))
    return false;
  recomp_func_t *const original =
      jfg_generated_lookup_function(static_cast<std::int32_t>(target));
  if (original == nullptr)
    return false;
  const auto write = [&](const jfg::mod::HudAnchor value) {
    std::uint32_t at = 0U;
    if (!memory.read_u32(list, at))
      return;
    (void)hud_full_width_scissor(state, memory, at);  // Follow the primitive's own commands.
    const auto command = jfg::mod::hud_rect_align_command(
        value, hud_framebuffer_width(state));
    for (std::size_t index = 0U; index < command.size(); ++index)
      (void)memory.write_u32(at + static_cast<std::uint32_t>(index * 4U),
                             command[index]);
    (void)memory.write_u32(list, at + 16U);
    state.hud.set_scan_cursor(at + 16U);
  };
  write(anchor);
  original(rdram, context);
  write(jfg::mod::HudAnchor::none);
  return true;
}

// RGO frame trace (JFG_RGO_FRAME_TRACE=<path>): one TSV row per game frame,
// written at viFrameSync entry. Columns: VI count, the ticks (delayDat) the
// frame was simulated with, gVideoDeltaTime, emulated Count, guest
// instructions, host microseconds since the trace started, front-end mode and
// level number. Logic rate = rows per 60 VIs; see scripts/rgo_frame_rate.py.
void write_rgo_frame_trace(State &state, const std::uint8_t *rdram) {
  hle::GuestMemory memory({const_cast<std::uint8_t *>(rdram), kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t delay = 0U, level = 0U;
  (void)memory.read_u32(jfg::kDelayDat, delay);
  (void)memory.read_u32(0x800FB114U, level);
  const unsigned delta = rdram[0x000FECACU ^ 3U];
  const unsigned mode = rdram[0x000A51B0U ^ 3U];
  const auto now = std::chrono::steady_clock::now();
  if (state.rgo_frame_trace_start == std::chrono::steady_clock::time_point{})
    state.rgo_frame_trace_start = now;
  const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
      now - state.rgo_frame_trace_start).count();
  // busy_us: host time of the frame that just ended; true60: 0 off, 1 at
  // 60, 2 the governor holds 30.
  const unsigned true60 = !jfg::true_logic_60(state.video) || !state.video_managed ? 0U
      : state.true60_governor.holding_30() ? 2U : 1U;
  std::fprintf(state.rgo_frame_trace, "%llu\t%u\t%u\t%llu\t%llu\t%lld\t%u\t%u\t%u\t%u\t%llu\t%u\t%llu\t%llu\t%lld\n",
               static_cast<unsigned long long>(state.vi_retraces), delay, delta,
               static_cast<unsigned long long>(state.cpu_count),
               static_cast<unsigned long long>(state.observed_guest_instructions),
               static_cast<long long>(wall_us), mode, level, state.frame_busy_last_us, true60,
               static_cast<unsigned long long>(state.host_audio.underruns()),
               state.host_audio.queued_milliseconds(),
               static_cast<unsigned long long>(state.wall_floor_jumps),
               static_cast<unsigned long long>(state.wall_floor_resets),
               static_cast<long long>(state.audio_trim_ns / 1000));
}

// RGO fast clock: a guest thread that keeps polling an empty queue in a loop
// (osRecvMesg NOBLOCK, e.g. amWaitForMidiSync waiting for the music in
// cutscenes) waits for a device event: an interrupt delivers the message.
// Once one call site has polled the same empty queue kBusyPolls times in
// quick succession, advance Count to the earliest pending device deadline,
// exactly as the idle loop skip does; the event is serviced at the next
// instruction. Polls a frame apart (mainResetPressed, viFrameSync's check
// for pending VIs) are not waits and never skip: skipping there would
// stretch the frame and put cutscenes out of step with their music.
constexpr std::uint32_t kBusyPolls = 16U;
constexpr std::uint64_t kBusyPollWindow = 256U;  // Count ticks, ~4096 instructions

void skip_empty_poll(State &state, const std::uint32_t queue, const std::uint32_t caller) {
  if (state.mmio_trace == nullptr)
    return;
  jfg_fast_run_end(&state);
  auto &sites = state.empty_poll_sites;
  auto *site = &sites[0];
  for (auto &candidate : sites) {
    if (candidate.queue == queue && candidate.caller == caller) {
      site = &candidate;
      break;
    }
    if (candidate.last_count < site->last_count)
      site = &candidate;  // least recently polled: reused for a new site
  }
  if (site->queue != queue || site->caller != caller) {
    *site = {queue, caller, state.cpu_count, 0U};
  }
  site->streak = state.cpu_count - site->last_count <= kBusyPollWindow ? site->streak + 1U : 1U;
  site->last_count = state.cpu_count;
  if (site->streak < kBusyPolls)
    return;
  site->streak = 0U;
  auto &mmio = *state.mmio_trace;
  auto deadline = mmio.vi_next;
  const auto admit = [&](const std::optional<std::uint64_t> next) {
    if (next) deadline = (std::min)(deadline, *next);
  };
  admit(mmio.pi_dma.deadline()); admit(mmio.si_dma.deadline());
  admit(state.guest_timer.deadline()); admit(mmio.sp_deadline);
  admit(mmio.dp_deadline); admit(mmio.ai_dma.deadline());
  if (deadline > state.cpu_count) {
    state.cpu_count = deadline;
    mmio.guest_count = state.cpu_count;
    site->last_count = state.cpu_count;
    ++state.empty_poll_skips;
  }
}

// RGO fast clock real-time floor. CPU work costs almost no emulated time, so
// a host too slow for a frame would otherwise stretch emulated time and play
// the game in slow motion. Instead, keep Count no further behind the wall
// clock than kWallSlack (anchored at the last paced VI): the next VI then
// arrives while the frame is still running, the game counts two VIs for it
// and simulates two ticks, as on the console and in other N64Recomp ports.
// Gameplay speed stays right; only the frame rate drops. Checked every few
// guest calls; a jump is serviced like the idle and poll skips.
//
// Following is bounded to kWallMaxLag. A longer stall (a shader compile, a
// window drag, loading on a slow PC) is not replayed as a burst of VIs: the
// anchor moves to now and the stalled time is skipped, as the audio clock
// does after an underrun. A burst would let the game's scheduler see its
// graphics task outlast ten VIs and stop with its "SP CRASHED" screen.
constexpr std::uint64_t kWallSlack = 781250U / 4U;   // a quarter VI, ~4 ms
constexpr std::uint64_t kWallMaxLag = 781250U * 3U;  // three VIs, 50 ms

void apply_wall_floor(State &state) {
  if (!state.wall_anchor_valid || state.mmio_trace == nullptr)
    return;
  jfg_fast_run_sync(&state);
  const auto now = std::chrono::steady_clock::now();
  if (now <= state.wall_anchor_time)
    return;
  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - state.wall_anchor_time).count());
  // Count runs at 46.875 MHz: 3 ticks per 64 ns.
  const std::uint64_t wall_count = state.wall_anchor_count + elapsed_ns * 3U / 64U;
  if (wall_count <= state.cpu_count + kWallSlack)
    return;
  if (wall_count > state.cpu_count + kWallMaxLag) {
    state.wall_anchor_count = state.cpu_count;
    state.wall_anchor_time = now;
    ++state.wall_floor_resets;
    return;
  }
  jfg_fast_run_end(&state);
  state.cpu_count = wall_count - kWallSlack;
  state.mmio_trace->guest_count = state.cpu_count;
  ++state.wall_floor_jumps;
}

// RGO: Frame rate went back to Original while playing. The game's own
// gVideoDeltaTime (0x800FECAC) is still 1 (60 fps) from true 60 and only
// returns to 2 on a level load, so restore its 30 fps mode now, unless the
// game itself runs this screen at 60 (runInOneFrame).
void leave_true60(State &state, std::uint8_t *rdram) {
  hle::GuestMemory memory({rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t one_frame = 0U;
  if (memory.read_u32(jfg::kRunInOneFrame, one_frame) && one_frame == 0U &&
      rdram[(jfg::kVideoDeltaTime - kKseg0) ^ 3U] == 1U)
    rdram[(jfg::kVideoDeltaTime - kKseg0) ^ 3U] = 2U;
  state.scroller_pending_ticks = 0U;
  state.half_rate_pending_ticks = 0U;
}

// RGO: how long the host was busy with the game frame that ends now: the wall
// time since the previous viFrameSync minus the time spent sleeping for a VI
// (0: not measured, e.g. the first frame or after the frontend paused).
std::uint32_t frame_busy_microseconds(State &state) {
  const auto now = std::chrono::steady_clock::now();
  const auto previous = state.busy_frame_start;
  const std::uint64_t all_waits =
      state.pacing_wait_us + state.host_audio.throttled_microseconds();
  const std::uint64_t waited = all_waits - state.busy_frame_wait_us;
  state.busy_frame_start = now;
  state.busy_frame_wait_us = all_waits;
  if (previous == std::chrono::steady_clock::time_point{})
    return 0U;
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count());
  const std::uint64_t busy = elapsed > waited ? elapsed - waited : 1U;
  state.frame_busy_us += busy;
  state.frame_busy_max_us = (std::max)(state.frame_busy_max_us, busy);
  ++state.frame_busy_frames;
  return static_cast<std::uint32_t>((std::min<std::uint64_t>)((std::max<std::uint64_t>)(busy, 1U),
                                                              UINT32_MAX));
}

// viFrameSync, once per game frame: counts the VI ticks the logic gets for
// the FPS counter's Detailed view. With true 60 fps logic (docs/rgo/framerate.md,
// Part E) it runs with the game's own runInOneFrame flag set, so it waits for
// one VI per frame and every tick function gets updateRate 1; the game's
// value is restored afterwards. Returns true when it ran the original.
bool frame_sync_dispatch(State &state, std::uint8_t *rdram,
                         recomp_context *context, const bool one_vi) {
  hle::GuestMemory memory({rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  recomp_func_t *const original =
      jfg_generated_lookup_function(static_cast<std::int32_t>(jfg::kViFrameSync));
  std::uint32_t saved = 0U;
  if (original == nullptr || !memory.read_u32(jfg::kRunInOneFrame, saved))
    return false;
  if (one_vi)
    (void)memory.write_u32(jfg::kRunInOneFrame, 1U);
  // RGO fast clock: a frame the host was too slow for (a load, a hitch) takes
  // an extra VI and its ticks, keeping the game's speed. Without this the game
  // would also drop to its 20 fps mode, which it leaves only after 31 fast
  // frames: one hitch would cost at least 1.5 s at 20 fps. The console's own
  // timing (Original N64 slowdown On) keeps that behaviour.
  if (state.fast_clock != 0U)
    rdram[(jfg::kSkipVideoDeltaAdjust - kKseg0) ^ 3U] = 1U;
  // Host time the frame that just ended took, without the VI waits
  // (measured when dispatch saw this viFrameSync call).
  const std::uint32_t busy_us = state.frame_busy_last_us;
  original(rdram, context);
  if (one_vi) {
    (void)memory.write_u32(jfg::kRunInOneFrame, saved);
    // This frame's ticks for the half-rate functions: every other frame, two
    // at once.
    state.half_rate_ticks = jfg::batch_ticks(state.half_rate_pending_ticks,
                                         static_cast<std::uint32_t>(context->r2));
  }
  // The governor sees every frame while True 60 is chosen, at 60 and while it
  // holds 30, so it can tell when the host is fast enough to resume.
  if (jfg::true_logic_60(state.video))
    state.true60_governor.frame(static_cast<std::uint32_t>(context->r2), state.vi_retraces,
                                busy_us);
  state.frame_logic_ticks += (std::min)(static_cast<std::uint32_t>(context->r2), 6U);
  return true;
}

// RGO true 60: guest bytes and halfwords for the odd-tick compensations
// (native word layout; the caller checks the range).
std::uint8_t guest_u8(const std::uint8_t *rdram, const std::uint32_t address) {
  return rdram[((address & 0x1FFFFFFFU) ^ 3U)];
}
void guest_put8(std::uint8_t *rdram, const std::uint32_t address, const std::uint8_t value) {
  rdram[((address & 0x1FFFFFFFU) ^ 3U)] = value;
}
std::uint16_t guest_u16(const std::uint8_t *rdram, const std::uint32_t address) {
  return static_cast<std::uint16_t>((guest_u8(rdram, address) << 8U) |
                                    guest_u8(rdram, address + 1U));
}
void guest_put16(std::uint8_t *rdram, const std::uint32_t address, const std::uint16_t value) {
  guest_put8(rdram, address, static_cast<std::uint8_t>(value >> 8U));
  guest_put8(rdram, address + 1U, static_cast<std::uint8_t>(value));
}
bool guest_range(const std::uint32_t address, const std::uint32_t bytes) {
  const std::uint32_t segment = address & 0xE0000000U;
  const std::uint32_t offset = address & 0x1FFFFFFFU;
  return (segment == 0x80000000U || segment == 0xA0000000U) && offset <= kRdramSize &&
         bytes <= kRdramSize - offset;
}

// RGO true 60: run a function that counts per call on a frame the half-rate
// functions skip, with its per-call step undone (frame_interpolation.hpp).
void odd_tick_compensated_call(const std::uint32_t target, std::uint8_t *rdram,
                               recomp_context *context, recomp_func_t *original) {
  if (target == jfg::kControlUpdatePlayerAim) {
    // Raise the countdowns by one; the call takes them back down.
    const auto control = static_cast<std::uint32_t>(context->r5);
    if (guest_range(control, 0x542U)) {
      const jfg::AimCounters counters = jfg::aim_counters_for_odd_tick(
          {static_cast<std::int8_t>(guest_u8(rdram, control + 0x1F4U)),
           static_cast<std::int8_t>(guest_u8(rdram, control + 0x1F8U)),
           guest_u8(rdram, control + 0x541U)});
      guest_put8(rdram, control + 0x1F4U, static_cast<std::uint8_t>(counters.hold));
      guest_put8(rdram, control + 0x1F8U, static_cast<std::uint8_t>(counters.spin_up));
      guest_put8(rdram, control + 0x541U, counters.lock);
    }
    original(rdram, context);
    return;
  }
  if (const jfg::StateCounter *const counter = jfg::state_counter(target)) {
    // An object control that steps one counter per call: take a one step back.
    const auto object = static_cast<std::uint32_t>(context->r4);
    std::uint32_t state_address = 0U;
    if (guest_range(object, 0x6CU))
      state_address = (static_cast<std::uint32_t>(guest_u16(rdram, object + 0x68U)) << 16U) |
                      guest_u16(rdram, object + 0x6AU);
    const std::uint32_t field = state_address + counter->offset;
    const bool valid = guest_range(object, 0x6CU) && guest_range(field, counter->bytes) &&
                       (counter->bytes == 1U || (field & 1U) == 0U);
    const auto read = [&]() -> std::uint32_t {
      return counter->bytes == 1U ? guest_u8(rdram, field) : guest_u16(rdram, field);
    };
    const std::uint32_t before = valid ? read() : 0U;
    original(rdram, context);
    if (valid && jfg::counter_stepped(before, read(), counter->bytes)) {
      if (counter->bytes == 1U)
        guest_put8(rdram, field, static_cast<std::uint8_t>(before));
      else
        guest_put16(rdram, field, static_cast<std::uint16_t>(before));
    }
    return;
  }
  if (target == jfg::kShirukenControl) {
    // Take back the spin step, and the tumble step after a bounce, when the
    // call made exactly those steps.
    const auto object = static_cast<std::uint32_t>(context->r4);
    std::uint32_t shuriken = 0U;
    if (guest_range(object, 0x6CU))
      shuriken = (static_cast<std::uint32_t>(guest_u16(rdram, object + 0x68U)) << 16U) |
                 guest_u16(rdram, object + 0x6AU);
    if (!guest_range(object, 0x6CU) || (shuriken & 3U) != 0U || !guest_range(shuriken, 0x84U)) {
      original(rdram, context);
      return;
    }
    const std::uint16_t spin = guest_u16(rdram, shuriken + 0x78U);
    const std::uint8_t flags = guest_u8(rdram, shuriken + 0x82U);
    std::array<std::uint16_t, 3> rotation{}, tumble{};
    for (std::uint32_t axis = 0U; axis < 3U; ++axis) {
      rotation[axis] = guest_u16(rdram, object + axis * 2U);
      tumble[axis] = guest_u16(rdram, shuriken + 0x7AU + axis * 2U);
    }
    original(rdram, context);
    if (jfg::angle_stepped(spin, guest_u16(rdram, shuriken + 0x78U), jfg::kShirukenSpinStep))
      guest_put16(rdram, shuriken + 0x78U, spin);
    if ((flags & 0x20U) != 0U && (guest_u8(rdram, shuriken + 0x82U) & 0x20U) != 0U) {
      bool stepped = true;
      for (std::uint32_t axis = 0U; axis < 3U; ++axis)
        stepped = stepped && guest_u16(rdram, shuriken + 0x7AU + axis * 2U) == tumble[axis] &&
            jfg::angle_stepped(rotation[axis], guest_u16(rdram, object + axis * 2U), tumble[axis]);
      if (stepped)
        for (std::uint32_t axis = 0U; axis < 3U; ++axis)
          guest_put16(rdram, object + axis * 2U, rotation[axis]);
    }
    return;
  }
  // camlightVisibilityCheck: keep the lens flare fades and the round-robin
  // cursor as they were; its z-buffer checks still run.
  std::array<std::uint8_t, jfg::kCamlightCount> fades{};
  std::array<std::uint8_t, 4> cursor{};
  for (std::uint32_t light = 0U; light < jfg::kCamlightCount; ++light)
    fades[light] = guest_u8(rdram, jfg::kCamlights + light * jfg::kCamlightStride +
                                       jfg::kCamlightFadeOffset);
  for (std::uint32_t byte = 0U; byte < 4U; ++byte)
    cursor[byte] = guest_u8(rdram, jfg::kCamlightCursor + byte);
  original(rdram, context);
  for (std::uint32_t light = 0U; light < jfg::kCamlightCount; ++light)
    guest_put8(rdram, jfg::kCamlights + light * jfg::kCamlightStride + jfg::kCamlightFadeOffset,
               fades[light]);
  for (std::uint32_t byte = 0U; byte < 4U; ++byte)
    guest_put8(rdram, jfg::kCamlightCursor + byte, cursor[byte]);
}

// RGO true 60 fps logic: the scheduler releases each frame's done message
// without waiting for a second VI, and the places that assume 2 ticks per
// frame are adjusted (frame_interpolation.hpp): texture scrollers and the
// bug AI run once per 2 ticks, literal 2-tick particle bursts use the real
// rate, and a frame without a new controller read reports no presses.
// Returns true when it ran (or deliberately skipped) the original.
bool true60_dispatch(State &state, const std::uint32_t target,
                     std::uint8_t *rdram, recomp_context *context) {
  if (target != jfg::kAnimseqUpdateTextureScrollers &&
      target != jfg::kPartUpdateTriggers && target != jfg::kJoyRead &&
      target != jfg::kScTaskComplete && !jfg::is_half_rate_function(target) &&
      !jfg::odd_tick_compensated(target))
    return false;
  hle::GuestMemory memory({rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  recomp_func_t *const original =
      jfg_generated_lookup_function(static_cast<std::int32_t>(target));
  if (original == nullptr)
    return false;
  if (target == jfg::kScTaskComplete) {
    // Release the frame-done message without waiting for a second VI.
    const auto frame_count_address =
        static_cast<std::uint32_t>(context->r4) + jfg::kSchedFrameCountOffset;
    std::uint32_t frame_count = 0U;
    if (!memory.read_u32(frame_count_address, frame_count))
      return false;
    (void)memory.write_u32(frame_count_address, jfg::true60_frame_count(frame_count));
    original(rdram, context);
    return true;
  }
  if (const jfg::HalfRateFunction *const half_rate = jfg::half_rate_function(target)) {
    // Game code that counts per call (the bug AI and others,
    // frame_interpolation.hpp): run it every other frame with both ticks.
    // Only the per-frame calls, which pass the frame's rate; level setup
    // callers run it as it is.
    std::uint32_t frame_rate = 0U;
    auto &rate = half_rate->rate_register == 4U ? context->r4
               : half_rate->rate_register == 5U ? context->r5
               : half_rate->rate_register == 6U ? context->r6 : context->r7;
    if (!memory.read_u32(jfg::kDelayDat, frame_rate) ||
        !jfg::half_rate_call(*half_rate, static_cast<std::uint32_t>(rate), frame_rate,
                             static_cast<std::uint32_t>(context->r31)))
      return false;
    if (target == jfg::kSquaddieControl)
      ++state.squaddie_calls;
    if (state.half_rate_ticks == 0U)
      return true;  // Skipped: runs with the next frame's tick.
    rate = state.half_rate_ticks;
    original(rdram, context);
    return true;
  }
  if (jfg::odd_tick_compensated(target)) {
    // Per-call steps that must still run every frame: on a frame the
    // half-rate functions skip, take the step back (frame_interpolation.hpp).
    if (state.half_rate_ticks != 0U)
      return false;  // An even tick: the call's own step is the 30 fps one.
    odd_tick_compensated_call(target, rdram, context, original);
    return true;
  }
  if (target == jfg::kAnimseqUpdateTextureScrollers) {
    const auto rate = static_cast<std::uint32_t>(context->r4);
    if (rate > 6U)
      return false;  // Not the rate argument this hook expects.
    const std::uint32_t run = jfg::batch_ticks(state.scroller_pending_ticks, rate);
    if (run == 0U)
      return true;  // Skipped: these ticks run with the next frame's.
    context->r4 = run;
    original(rdram, context);
    return true;
  }
  if (target == jfg::kPartUpdateTriggers) {
    std::uint32_t frame_rate = 0U;
    if (!memory.read_u32(jfg::kDelayDat, frame_rate))
      return false;
    context->r5 = jfg::trigger_ticks(static_cast<std::uint32_t>(context->r5), frame_rate);
    original(rdram, context);
    return true;
  }
  // joyRead: compare both controller reads before and after the call.
  constexpr std::size_t kWords = jfg::kControllerPadBytes * 2U / 4U;
  std::array<std::uint8_t, 48> before{}, after{};
  auto snapshot = [&](std::array<std::uint8_t, 48>& bytes) {
    for (std::size_t index = 0U; index < kWords; ++index) {
      std::uint32_t word = 0U;
      if (!memory.read_u32(jfg::kControllerCurrent + static_cast<std::uint32_t>(index * 4U), word))
        return false;
      for (std::size_t byte = 0U; byte < 4U; ++byte)
        bytes[index * 4U + byte] = static_cast<std::uint8_t>(word >> (24U - byte * 8U));
    }
    return true;
  };
  if (!snapshot(before))
    return false;
  original(rdram, context);
  if (snapshot(after) && !jfg::controller_read_consumed(before, after))
    for (std::uint32_t offset = 0U; offset < 8U; offset += 4U) {
      (void)memory.write_u32(jfg::kControllerPressed + offset, 0U);
      (void)memory.write_u32(jfg::kControllerReleased + offset, 0U);
    }
  return true;
}

// RGO frame interpolation, phase 2 (experimental, interpolate_objects=on):
// run the game's object matrix writers and tag each matrix they write with an
// ID from the object pointer and how many matrices it pushed since the last
// camera. The renderer checks the tag against the matrix words, so tags for
// reused matrix memory are ignored. Returns true when it ran the original.
bool interpolation_tag_dispatch(State &state, const std::uint32_t target,
                                std::uint8_t *rdram, recomp_context *context) {
  if (target == jfg::kCamSetProjMtx) {
    // A new camera: objects are drawn again for it (split screen).
    state.object_matrix_pushes.clear();
    return false;
  }
  if (target != jfg::kCamPushModelMtx && target != jfg::kCamPushFloatModelMtx &&
      target != jfg::kCamPushMuzzleMtx)
    return false;
  hle::GuestMemory memory({rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  std::uint32_t cursor = 0U;
  if (!memory.read_u32(static_cast<std::uint32_t>(context->r5), cursor))
    return false;
  const std::uint32_t key =
      jfg::object_key(target, static_cast<std::uint32_t>(context->r6));
  recomp_func_t *const original =
      jfg_generated_lookup_function(static_cast<std::int32_t>(target));
  if (original == nullptr)
    return false;
  original(rdram, context);
  const std::uint32_t matrix = (cursor & 0x1FFFFFFFU) | kKseg0;
  std::array<std::uint32_t, 16> words{};
  for (std::size_t index = 0U; index < words.size(); ++index)
    if (!memory.read_u32(matrix + static_cast<std::uint32_t>(index * 4U), words[index]))
      return true;
  if (state.object_matrix_pushes.size() > 4096U)
    state.object_matrix_pushes.clear();
  const std::uint32_t push = state.object_matrix_pushes[key]++;
  state.rt64_shell->tag_matrix(matrix & 0x00FFFFFFU, jfg::matrix_words_hash(words),
                               jfg::object_group_id(key, push));
  return true;
}

// Returns true when it ran the original function itself.
bool widescreen_hud_dispatch(State &state, const std::uint32_t target,
                             std::uint8_t *rdram, recomp_context *context) {
  using namespace jfg::mod;
  hle::GuestMemory memory({rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  if (target == kHudFrontUpdate.address) {
    std::uint32_t start = 0U;
    state.hud_anchoring = state.widescreen_extent > 1.0F &&
        hud_qualified(memory, kHudFrontUpdate) &&
        memory.read_u32(static_cast<std::uint32_t>(context->r4), start);
    state.hud.begin_frame(state.hud_anchoring,
                          static_cast<float>(hud_framebuffer_width(state)),
                          state.hud_anchoring ? start : 0U);
    return false;
  }
  if (!state.hud_anchoring || !state.hud.in_group())
    return false;
  const float width = static_cast<float>(hud_framebuffer_width(state));
  if (target == kHudFontPrintXY.address && hud_qualified(memory, kHudFontPrintXY)) {
    const auto x = static_cast<std::int32_t>(context->r5);
    const float position = x == kHudPositionCentred ? width * 0.5F
                                                    : static_cast<float>(x);
    state.hud.add(position, position);
    return hud_aligned_call(state, memory, static_cast<std::uint32_t>(context->r4),
                            target, rdram, context);
  }
  if (target == kHudDrawRectangles.address &&
      hud_qualified(memory, kHudDrawRectangles)) {
    const auto count = static_cast<std::int32_t>(context->r5);
    const auto rectangles = static_cast<std::uint32_t>(context->r6);
    for (std::int32_t index = 0; index >= 0 && index < count && index < 64; ++index) {
      const std::uint32_t entry = rectangles +
          static_cast<std::uint32_t>(index) * kHudRectangleStride;
      std::uint32_t horizontal = 0U, vertical = 0U;
      if (!memory.read_u32(entry, horizontal) ||
          !memory.read_u32(entry + 4U, vertical))
        break;
      const auto left = static_cast<std::int16_t>(horizontal >> 16U);
      const auto right = static_cast<std::int16_t>(vertical >> 16U);
      if (right >= 0 && static_cast<std::int16_t>(vertical) >= 0)
        state.hud.add(static_cast<float>(left), static_cast<float>(right));
    }
    return hud_aligned_call(state, memory, static_cast<std::uint32_t>(context->r4),
                            target, rdram, context);
  }
  if (target == kHudDrawObj.address && hud_qualified(memory, kHudDrawObj)) {
    const auto index = static_cast<std::uint32_t>(context->r4);
    const std::uint32_t field = kHudCurrentObjects +
        index * kHudCurrentObjectStride + kHudCurrentObjectX;
    std::uint32_t bits = 0U, cursor = 0U;
    if (index >= kHudCurrentObjectCount || !memory.read_u32(field, bits))
      return false;
    const float x = std::bit_cast<float>(bits);
    if (!std::isfinite(x))
      return false;
    state.hud.add(width * 0.5F + x, width * 0.5F + x);
    const HudAnchor anchor = state.hud.anchor();
    if (anchor == HudAnchor::none || !memory.read_u32(kHudFrontGfx, cursor) ||
        !hud_full_width_scissor(state, memory, cursor))
      return false;
    recomp_func_t *const original =
        jfg_generated_lookup_function(static_cast<std::int32_t>(target));
    if (original == nullptr)
      return false;
    const float shifted = x + hud_object_shift(
        anchor, hud_extent(state.widescreen_extent), width);
    (void)memory.write_u32(field, std::bit_cast<std::uint32_t>(shifted));
    original(rdram, context);
    (void)memory.write_u32(field, bits);
    return true;
  }
  return false;
}
#endif

int dispatch(void *opaque, std::int32_t address, std::uint8_t *rdram,
             recomp_context *context) {
  auto &state = *static_cast<State *>(opaque);
  const auto target = static_cast<std::uint32_t>(address);
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  for (std::size_t watch = 0U;
       !state.progress_path.empty() &&
       watch < state.matrix_watch_addresses.size(); ++watch) {
    const std::uint32_t watched = state.matrix_watch_addresses[watch];
    if (watched == 0U)
      continue;
    // These observations use the native-word layout. Validate the same
    // aligned RDRAM range before loading directly, avoiding an out-of-line
    // GuestMemory call for each of the eight words on every dispatch.
    if ((watched & 3U) != 0U || watched < kKseg0 ||
        watched - kKseg0 > kRdramSize - sizeof(std::uint32_t))
      continue;
    std::uint32_t current = 0U;
    std::memcpy(&current, rdram + (watched - kKseg0), sizeof(current));
    if (current == state.matrix_watch_words[watch])
      continue;
    const std::size_t slot = state.recent_matrix_watch_position %
        state.recent_matrix_watch_addresses.size();
    state.recent_matrix_watch_addresses[slot] = watched;
    state.recent_matrix_watch_before[slot] = state.matrix_watch_words[watch];
    state.recent_matrix_watch_after[slot] = current;
    state.recent_matrix_watch_targets[slot] =
        state.recent_dispatch_position == 0U
            ? 0U
            : state.recent_dispatches[(state.recent_dispatch_position - 1U) %
                                      state.recent_dispatches.size()];
    state.recent_matrix_watch_callers[slot] =
        state.recent_dispatch_position < 2U
            ? 0U
            : state.recent_dispatches[(state.recent_dispatch_position - 2U) %
                                      state.recent_dispatches.size()];
    state.recent_matrix_watch_current_targets[slot] = target;
    state.recent_matrix_watch_return_addresses[slot] =
        static_cast<std::uint32_t>(context->r31);
    if (watched == 0x800FAF7CU) {
      state.poison_matrix_watch = {
          state.matrix_watch_words[watch], current,
          state.recent_matrix_watch_targets[slot],
          state.recent_matrix_watch_callers[slot], target,
          state.recent_matrix_watch_return_addresses[slot]};
      const std::size_t history_count =
          (std::min)(state.recent_dispatch_position,
                     state.poison_dispatch_history.size());
      state.poison_dispatch_history.fill(0U);
      for (std::size_t index = 0U; index < history_count; ++index) {
        const std::size_t distance = history_count - index;
        state.poison_dispatch_history[index] =
            state.recent_dispatches[(state.recent_dispatch_position - distance) %
                                    state.recent_dispatches.size()];
      }
    }
    ++state.recent_matrix_watch_position;
    state.matrix_watch_words[watch] = current;
  }
#endif
  if (target == 0xFFFFFFFEU) {
    if (state.scheduler == nullptr || !state.scheduler_running ||
        state.scheduler->current_thread_id() < 0) {
      fail_closed_dispatch(state, "scheduler", "pause-outside-thread",
                           target);
    }
    state.journal.append(NativeEventKind::kThreadPaused, target,
                         static_cast<std::uint32_t>(
                             state.scheduler->current_thread_id()));
    state.scheduler->pause_current();
    fail_closed_dispatch(state, "scheduler", "pause-resumed", target);
  }
  if (state.mmio_trace != nullptr &&
      state.mmio_trace->unsupported_accesses != 0U)
    fail_closed_dispatch(state, "mmio", "unsupported-register", target);
  ++state.dispatch_calls;
  // RGO fast clock: the host code below sees an up-to-date Count.
  jfg_fast_run_sync(&state);
#if defined(JFG_LIVE_RUNTIME)
  // RGO widescreen: the track renderer culls level blocks against fixed
  // camera-space side planes sized for the original picture. Open them by
  // the presented horizontal extent while func_800158BC builds the planes,
  // then restore the table. Qualify the routine by its prologue.
  if (target == 0x800158BCU && state.widescreen_extent > 1.0F) {
    hle::GuestMemory memory({rdram, kRdramSize},
                           hle::GuestMemory::Layout::native_word_big_endian);
    constexpr auto triangles = jfg::kTrackFrustumTrianglesAddress;
    std::array<std::uint32_t, jfg::kTrackFrustumFloatCount> saved{};
    std::uint32_t first = 0U, second = 0U;
    bool qualified = memory.read_u32(target, first) &&
        memory.read_u32(target + 4U, second) && first == 0x27BDFF30U &&
        second == 0xAFB1005CU;
    for (std::size_t index = 0U; qualified && index < saved.size(); ++index)
      qualified = memory.read_u32(
          triangles + static_cast<std::uint32_t>(index * 4U), saved[index]);
    recomp_func_t *const original = qualified
        ? jfg_generated_lookup_function(static_cast<std::int32_t>(target))
        : nullptr;
    if (original) {
      std::array<float, jfg::kTrackFrustumFloatCount> widened{};
      for (std::size_t index = 0U; index < saved.size(); ++index)
        widened[index] = std::bit_cast<float>(saved[index]);
      jfg::widen_track_frustum(widened, state.widescreen_extent);
      for (std::size_t index = 0U; index < saved.size(); ++index)
        (void)memory.write_u32(triangles + static_cast<std::uint32_t>(index * 4U),
                               std::bit_cast<std::uint32_t>(widened[index]));
      original(rdram, context);
      for (std::size_t index = 0U; index < saved.size(); ++index)
        (void)memory.write_u32(triangles + static_cast<std::uint32_t>(index * 4U),
                               saved[index]);
      return 1;
    }
  }
  // RGO frame trace and FPS counter: viFrameSync starts every game frame.
  // delayDat still holds the ticks the frame that just ended was simulated
  // with.
  if (target == jfg::kViFrameSync) {
    ++state.logic_frames;
    state.frame_busy_last_us = frame_busy_microseconds(state);
    if (state.rgo_frame_trace != nullptr)
      write_rgo_frame_trace(state, rdram);
    // A new level or cutscene: True 60 tries 60 again at once.
    std::uint32_t level = 0U;
    std::memcpy(&level, rdram + (jfg::kCurrentLevel - kKseg0), sizeof(level));
    if (level != state.true60_level) {
      state.true60_level = level;
      state.true60_governor.new_scene(state.vi_retraces);
    }
  }
  // RGO true 60 fps logic (Frame rate above Original). Replays and captures
  // keep the game's own frame pacing.
  const bool true60 = state.video_managed && !state.input_replay_loaded &&
      jfg::true_logic_60(state.video) && state.true60_governor.active(state.vi_retraces);
  if (target == jfg::kViFrameSync) {
    if (state.true60_active && !true60)
      leave_true60(state, rdram);
    state.true60_active = true60;
  }
  if (state.fast_clock != 0U && !state.wall_floor_disabled &&
      (++state.wall_floor_calls & 15U) == 0U)
    apply_wall_floor(state);
  // viFrameSync: the Detailed counter's ticks, and at true 60 one VI per frame.
  if (target == jfg::kViFrameSync && state.video_managed && !state.input_replay_loaded &&
      frame_sync_dispatch(state, rdram, context, true60))
    return 1;
  if (true60 && true60_dispatch(state, target, rdram, context))
    return 1;
  // RGO frame interpolation: tag object matrices (experimental).
  if (state.rt64_shell != nullptr && state.video.interpolate_objects &&
      jfg::interpolation_requested(state.video) &&
      interpolation_tag_dispatch(state, target, rdram, context))
    return 1;
  // RGO widescreen HUD: track the HUD frame and its element groups, and
  // anchor elements drawn by the 2D primitives.
  std::optional<HudGroupScope> hud_group;
  if (state.hud_anchoring && jfg::mod::is_hud_group(target))
    hud_group.emplace(state.hud, target);
  if ((state.hud_anchoring || target == jfg::mod::kHudFrontUpdate.address) &&
      widescreen_hud_dispatch(state, target, rdram, context))
    return 1;
  // The normal camera has computed its desired point, but has not yet run
  // scenery constraints. Feed the orbit into that existing solver, never over
  // its final position. Qualify both the owning routine and this call site.
  if(target==0x800446E4U && state.pc_camera_actor!=0 &&
     static_cast<std::uint32_t>(context->r29)==state.pc_camera_stack) {
    jfg::mod::Memory ram({rdram,kRdramSize});
    const auto stack=static_cast<std::uint32_t>(context->r29);
    const auto actor=state.pc_camera_actor,control=state.pc_camera_control;
    const auto port=ram.u8(control);
    if(port<4 && ram.valid(stack,0xF4U) && ram.u32(stack+0xF0U)==actor && ram.u32(0x8002E158U)==0x0C0111B9U) {
      auto &orbit=state.pc_orbits[port];
      const auto camera=ram.u32(0x800F6DC0U);
      const float px=ram.f32(actor+0xCU),py=ram.f32(actor+0x10U)+40.0F,pz=ram.f32(actor+0x14U);
      const float dx=ram.f32(stack+0x8CU)-px,dy=ram.f32(stack+0x90U)-py,dz=ram.f32(stack+0x94U)-pz;
      const float radius=std::sqrt(dx*dx+dy*dy+dz*dz);
      if(ram.valid(camera,0x4CU) && std::isfinite(radius) && radius>=40.0F && radius<=1000.0F) {
        if(!orbit.active || orbit.actor!=actor || state.vi_retraces-orbit.retrace>15U) {
          orbit.yaw=ram.s16(camera);
          orbit.pitch=static_cast<int>(std::lround(std::atan2(dy,std::sqrt(dx*dx+dz*dz))*65536.0F/6.283185307179586F));
        }
        orbit.active=true;orbit.actor=actor;orbit.retrace=state.vi_retraces;
        orbit.yaw=jfg::pc_wrap_angle(orbit.yaw+state.pc_camera_look.x);
        orbit.pitch=std::clamp(orbit.pitch-state.pc_camera_look.y,-8192,12288);
        const auto point=jfg::pc_orbit_point(orbit.yaw,orbit.pitch,radius);
        hle::GuestMemory memory({rdram,kRdramSize},hle::GuestMemory::Layout::native_word_big_endian);
        (void)memory.write_u32(stack+0x8CU,std::bit_cast<std::uint32_t>(px+point.x));
        (void)memory.write_u32(stack+0x90U,std::bit_cast<std::uint32_t>(py+point.y));
        (void)memory.write_u32(stack+0x94U,std::bit_cast<std::uint32_t>(pz+point.z));
        state.gameplay_trace.event("pc-orbit",{state.vi_retraces,state.controller_samples,port,
          static_cast<std::uint16_t>(orbit.yaw),static_cast<std::uint16_t>(orbit.pitch),
          std::bit_cast<std::uint32_t>(radius),static_cast<std::uint32_t>(state.pc_camera_look.x),
          static_cast<std::uint32_t>(state.pc_camera_look.y)});
      }
    }
  }
  // The scenery solver can leave its point exactly on a wall. Keep a small
  // near-plane clearance toward its focus, only when collision moved the
  // requested orbit point. Preserve the solver's unblocked result verbatim.
  if(target==0x8002CC70U && state.pc_camera_actor!=0 &&
     static_cast<std::uint32_t>(context->r29)==state.pc_camera_stack) {
    jfg::mod::Memory ram({rdram,kRdramSize});
    const auto focus=static_cast<std::uint32_t>(context->r4),point=static_cast<std::uint32_t>(context->r5);
    const auto stack=state.pc_camera_stack;
    if(focus==stack+0x98U && point==stack+0x8CU && ram.valid(stack,0xA4U) &&
       ram.u32(target)==0x27BDFF60U && ram.u32(target+4U)==0xAFBF002CU) {
      const jfg::PcOrbitPoint from{ram.f32(focus),ram.f32(focus+4U),ram.f32(focus+8U)};
      const jfg::PcOrbitPoint wanted{ram.f32(point),ram.f32(point+4U),ram.f32(point+8U)};
      auto* original=jfg_generated_lookup_function(static_cast<std::int32_t>(target));
      if(original) {
        original(rdram,context);
        const jfg::PcOrbitPoint resolved{ram.f32(point),ram.f32(point+4U),ram.f32(point+8U)};
        const auto clear=jfg::pc_camera_clearance(from,wanted,resolved);
        hle::GuestMemory memory({rdram,kRdramSize},hle::GuestMemory::Layout::native_word_big_endian);
        (void)memory.write_u32(point,std::bit_cast<std::uint32_t>(clear.x));
        (void)memory.write_u32(point+4U,std::bit_cast<std::uint32_t>(clear.y));
        (void)memory.write_u32(point+8U,std::bit_cast<std::uint32_t>(clear.z));
        state.gameplay_trace.event("pc-camera-clearance",{state.vi_retraces,
          std::bit_cast<std::uint32_t>(wanted.x),std::bit_cast<std::uint32_t>(wanted.y),std::bit_cast<std::uint32_t>(wanted.z),
          std::bit_cast<std::uint32_t>(resolved.x),std::bit_cast<std::uint32_t>(resolved.y),std::bit_cast<std::uint32_t>(resolved.z),
          std::bit_cast<std::uint32_t>(clear.x),std::bit_cast<std::uint32_t>(clear.y),std::bit_cast<std::uint32_t>(clear.z)});
        return 1;
      }
    }
  }
  if(target==0x8002CF6CU && state.pc_experimental && (state.play_mode || state.input_replay_loaded) &&
     (state.input_record_path.empty() || state.pc_record_extended) && !state.pc_input_blocked &&
     state.pc_camera_actor==0) {
    jfg::mod::Memory ram({rdram,kRdramSize});
    const auto actor=static_cast<std::uint32_t>(context->r4),control=static_cast<std::uint32_t>(context->r5);
    if(ram.valid(control,0x5C4U) && ram.valid(actor,0xA4U) && ram.u32(actor+0x68U)==control &&
       ram.u32(target)==0x27BDFF10U && ram.u32(target+4)==0xAFBF002CU &&
       jfg::mod::gameplay_active(ram,actor) && ram.u8(0x800FD7BDU)==0 && !jfg::mod::scripted_camera_active(ram,actor)) {
      const auto port=ram.u8(control);
      if(port<4 && state.controller_ports.samples[port].connected) {
        const auto &frame=state.pc_frames[port];
        if((frame.pc_mode==3 || frame.pc_mode==4) && !(state.controller_ports.samples[port].buttons&0x10)) {
          auto* original=jfg_generated_lookup_function(static_cast<std::int32_t>(target));
          if(original) {
            auto look=jfg::PcLookDelta{frame.look_x,frame.look_y};
            if(frame.pc_mode==3) {
              if(state.pc_mouse_used[port])look={};
              state.pc_mouse_used[port]=true;
            }
            state.pc_camera_actor=actor;state.pc_camera_control=control;
            state.pc_camera_stack=static_cast<std::uint32_t>(context->r29)-0xF0U;
            state.pc_camera_look=jfg::pc_add_look(jfg::pc_camera_delta(frame.pc_mode,look,
              std::bit_cast<float>(static_cast<std::uint32_t>(context->r6))),take_direct(state,port));
            original(rdram,context);
            state.pc_camera_actor=state.pc_camera_control=state.pc_camera_stack=0;state.pc_camera_look={};
            return 1;
          }
        } else state.pc_orbits[port].active=false;
      }
    }
  }
  if (target == 0x8004B0F4U || target == 0x8004B204U || target == 0x8004B29CU ||
      target == 0x8004B3D8U || target == 0x8004B518U)
    rumble_request(state, target, *context);
  // Separate look input is recorded alongside movement; older replays remain stock.
  if(target==0x8003AABCU && state.pc_experimental && (state.play_mode || state.input_replay_loaded) &&
     (state.input_record_path.empty() || state.pc_record_extended) && !state.pc_input_blocked) {
    jfg::mod::Memory pc_memory({rdram,kRdramSize});
    const auto actor=static_cast<std::uint32_t>(context->r4);
    const auto control=static_cast<std::uint32_t>(context->r5);
    if(pc_memory.valid(control,0x5C4U) && pc_memory.valid(actor,0xA4U) &&
       pc_memory.u32(actor+0x68U)==control && pc_memory.u32(target)==0x27BDFFA8U &&
       pc_memory.u32(target+4)==0xAFB00018U &&
       jfg::mod::gameplay_active(pc_memory,actor) && pc_memory.u8(0x800FD7BDU)==0 && !jfg::mod::scripted_camera_active(pc_memory,actor)) {
      const auto port=pc_memory.u8(control);
      if(port<4U && state.controller_ports.samples[port].connected) {
        const auto &frame=state.pc_frames[port];
        const bool mouse=frame.pc_mode==1 || frame.pc_mode==3 || frame.pc_mode==5;
        const bool direct=frame.pc_mode==3 || frame.pc_mode==4 || frame.pc_mode==5;
        const auto extra=take_direct(state,port); // RGO: gyro
        if(frame.pc_mode!=0 || extra.x || extra.y) {
          auto look=jfg::PcLookDelta{frame.look_x,frame.look_y};
          if(mouse && state.pc_mouse_used[port])look={};
          if(mouse)state.pc_mouse_used[port]=true;
          const auto stack=static_cast<std::uint32_t>(context->r29);
          const bool valid_limits=pc_memory.valid(stack,0x24U);
          if(frame.pc_mode==4)look=jfg::pc_camera_delta(frame.pc_mode,look,
            valid_limits?std::bit_cast<float>(pc_memory.u32(stack+0x20U)):0.0F);
          const int minimum=valid_limits?static_cast<std::int32_t>(pc_memory.u32(stack+0x10U)):0;
          const int maximum=valid_limits?static_cast<std::int32_t>(pc_memory.u32(stack+0x14U)):0;
          const auto before=jfg::PcLookDelta{pc_memory.s16(control+0x11CU),pc_memory.s16(control+0x1E2U)};
          hle::GuestMemory memory({rdram,kRdramSize},hle::GuestMemory::Layout::native_word_big_endian);
          std::uint32_t x=0,y=0;
          if(memory.read_u32(0x800F6DB4U,x) && memory.read_u32(0x800F6DB8U,y)) {
            auto* original=jfg_generated_lookup_function(static_cast<std::int32_t>(target));
            if(original) {
              // Stock aiming (mode 0, gyro only) keeps the game's own stick.
              if(frame.pc_mode!=0) {
                (void)memory.write_u32(0x800F6DB4U,direct?0U:static_cast<std::uint32_t>(look.x));
                (void)memory.write_u32(0x800F6DB8U,direct?0U:static_cast<std::uint32_t>(look.y));
              }
              original(rdram,context);
              if(frame.pc_mode!=0) {(void)memory.write_u32(0x800F6DB4U,x);(void)memory.write_u32(0x800F6DB8U,y);}
              ++state.pc_aim_calls;
              if(valid_limits && minimum>=-32768 && maximum<=32767 && minimum<=maximum) {
                if(direct) {
                  const auto after=jfg::pc_direct_aim(before.x,before.y,jfg::pc_add_look(look,extra),minimum,maximum);
                  pc_memory.put16(control+0x11CU,static_cast<std::uint16_t>(after.x));
                  pc_memory.put16(control+0x1E2U,static_cast<std::uint16_t>(after.y));
                  (void)memory.write_u32(control+0x1E4U,0);(void)memory.write_u32(control+0x1E8U,0);
                  ++state.pc_direct_aim_calls;
                } else if(extra.x || extra.y) {
                  // RGO: gyro on top of the game's own aiming: turn from where
                  // the game left the aim this update.
                  const auto after=jfg::pc_direct_aim(pc_memory.s16(control+0x11CU),pc_memory.s16(control+0x1E2U),
                    extra,minimum,maximum);
                  pc_memory.put16(control+0x11CU,static_cast<std::uint16_t>(after.x));
                  pc_memory.put16(control+0x1E2U,static_cast<std::uint16_t>(after.y));
                  ++state.pc_direct_aim_calls;
                }
              }
              // Ground aiming otherwise ignores the movement stick. The guest
              // consumes these local velocities in its ordinary physics/collision
              // path. Leave jumps, swimming, damage and scripted states alone.
              const auto movement_state=pc_memory.u8(control+0x568U);
              if((frame.pc_mode==3 || frame.pc_mode==4) && (movement_state==10 || movement_state==11) && (pc_memory.u8(control+1U)&3U)<3U) {
                const auto &move=state.controller_ports.samples[port];
                const auto velocity=jfg::pc_aim_movement(move.x,move.y);
                (void)memory.write_u32(control+0x10U,std::bit_cast<std::uint32_t>(velocity.side));
                (void)memory.write_u32(control+0x4U,std::bit_cast<std::uint32_t>(velocity.forward));
              }
              state.gameplay_trace.event("pc-aim",{state.vi_retraces,state.controller_samples,port,frame.pc_mode,
                static_cast<std::uint32_t>(look.x),static_cast<std::uint32_t>(look.y),
                pc_memory.u16(control+0x11CU),pc_memory.u16(control+0x1E2U),movement_state,
                pc_memory.u32(control+4U),pc_memory.u32(control+0x10U)});
              return 1;
            }
          }
        }
      }
    }
  }
  if (state.navigation_mod.observing()) {
    jfg::mod::Memory memory({rdram,kRdramSize});
    if (target==0x8004665CU) {
      state.navigation_mod.transition();
      // Invalidate the old map before the game starts loading another room.
      std::ostringstream output;
      state.navigation_mod.write_state(output,memory,{},UINT32_MAX,false);
      if(!write_mod_export(state, state.navigation_output/L"live.json",output.str()))
        ++state.navigation_mod.invalid;
    }
    if (target==0x80032A48U) {
      (void)state.navigation_mod.observe_player(memory,static_cast<std::uint32_t>(context->r4));
      (void)state.navigation_mod.full_health(memory,static_cast<std::uint32_t>(context->r4));
    }
    if (target==0x00310600U)
      (void)state.navigation_mod.clear_enemy(memory,static_cast<std::uint32_t>(context->r4));
  }
#endif
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  // Private fixture travel request; let mainChangeLevel prepare all globals.
  if (target == 0x8004665CU && state.timing_fixture) {
    if (state.timing_trace)
      state.timing_trace << "travel-request\t" << state.vi_frames << '\t'
          << context->r4 << '\t' << context->r5 << '\t'
          << context->r6 << '\t' << context->r7 << '\t' << context->r31 << '\n';
    if (context->r4 == 127U || context->r4 == 214U) context->r5 = 2U;
    if (context->r4 == 214U) {
      char value[16]{};
      const auto length = GetEnvironmentVariableA("QUARRY_FIXTURE_DESTINATION", value, 16U);
      const auto destination = length > 0U && length < 16U ? std::stoul(value) : 294U;
      if (destination != 294U && destination != 272U && destination != 283U && destination != 354U && destination != 411U && destination != 357U)
        fail_closed_dispatch(state, "diagnostic", "fixture-destination", target);
      context->r4 = destination;
      char group[16]{};
      const auto group_length = GetEnvironmentVariableA("QUARRY_FIXTURE_GROUP", group, 16U);
      if (group_length > 0U && group_length < 16U) context->r7 = std::stoul(group);


      if (state.timing_trace)
        state.timing_trace << "travel-fixture\t" << state.vi_frames << '\t'
            << destination << '\t' << context->r5 << '\t' << context->r6 << '\t'
            << context->r7 << '\n';
    }
  }
  switch (target) {
  case 0x020002E8U: // overlay 32: mrhintsControl / KingBear
    note_hints_control_call(state, target, *context);
    break;
  case 0x02002540U: // overlay 32: mrHintsTalk
    ++state.phase9_hints_talk_calls;
    break;
  case 0x00700148U: // overlay 7: hinttextStart
    ++state.phase9_hinttext_start_calls;
    break;
  case 0x00700D6CU: // overlay 7: hinttextStop
    ++state.phase9_hinttext_stop_calls;
    break;
  case 0x0070070CU: // overlay 7: advance queued hint page
    ++state.phase9_hinttext_advance_calls;
    break;
  case 0x00700D80U: // overlay 7: hinttextActive
    ++state.phase9_hinttext_active_calls;
    break;
  case 0x00701CCCU: // overlay 7: hinttextUpdate
    ++state.phase9_hinttext_update_calls;
    break;
  case 0x007016B4U: // overlay 7: accept/parse current hint page
    ++state.phase9_hinttext_accept_calls;
    break;
  case 0x80032A48U: // controlPlayer
    ++state.phase9_player_control_calls;
    state.phase9_player_actor = static_cast<std::uint32_t>(context->r4);
    break;
  case 0x80037550U: { // controlUpdateWeapon
    ++state.phase9_weapon_update_calls;
    hle::GuestMemory memory(
        {rdram, kRdramSize},
        hle::GuestMemory::Layout::native_word_big_endian);
    std::uint32_t held = 0U;
    std::uint32_t pressed = 0U;
    if (memory.read_u32(0x800F6DA0U, held) && (held & 0x2000U) != 0U)
      ++state.phase9_weapon_fire_held_updates;
    if (memory.read_u32(0x800F6DA4U, pressed) && (pressed & 0x2000U) != 0U)
      ++state.phase9_weapon_fire_pressed_updates;
    break;
  }
  case 0x800386B0U: // func_80038488 (shooting_func)
    ++state.phase9_weapon_fire_calls;
    break;
  case 0x80038630U: // controlFireDummyShot
    ++state.phase9_weapon_dummy_fire_calls;
    break;
  case 0x800478A4U: // mainIncreaseWeaponShots
    ++state.phase9_weapon_shot_calls;
    break;
  case 0x800478E0U: // mainIncreaseWeaponHits
    ++state.phase9_weapon_hit_calls;
    break;
  case 0x80047868U: // mainIncreaseWeaponKills
    ++state.phase9_enemy_kill_calls;
    break;
  case 0x800163FCU: // trackGetPlayerIntersect
  case 0x80034BB0U: // controlGroundHits
    ++state.phase9_collision_calls;
    break;
  case 0x80063358U: // partUpdateParticles
    ++state.phase9_particle_update_calls;
    break;
  case 0x8004665CU: // mainChangeLevel
    ++state.phase9_level_change_calls;
    break;
  case 0x80046378U: // mainRestartAfterDeath
    ++state.phase9_death_restart_calls;
    break;
  case 0x80079890U: // animseqCamera
    ++state.phase9_cutscene_camera_calls;
    break;
  case 0x8007E150U: // hitPlayer
    ++state.phase9_player_hit_check_calls;
    break;
  default:
    break;
  }
#endif
#if !defined(JFG_PHASE8_LIVE_RUNTIME)
  state.journal.append(NativeEventKind::kDispatch, target);
#endif
  if (!state.guest_os_probe) {
    state.cpu_count += kCountTicksPerDispatch;
    service_si_deadline(state, target);
    service_timers(state, target);
  }
  state.recent_dispatches[state.recent_dispatch_position %
                          state.recent_dispatches.size()] = target;
  ++state.recent_dispatch_position;
#if defined(JFG_LIVE_RUNTIME)
  if (state.vi_retraces >= 5683U && state.dispatch_calls % 1024U == 0U)
    write_private_progress(state, "dispatch");
#endif
  if (target == jfg::boot::kSyncPrintSink) {
    hle::GuestMemory memory({rdram, kRdramSize},
                           hle::GuestMemory::Layout::native_word_big_endian);
    if (!jfg::boot::invoke_sync_print_sink(memory, *context))
      fail_closed_dispatch(state, "guest-leaf", "sync-print-sink-state", target);
    ++state.sync_print_sink_calls;
    return 1;
  }
  const auto mapping = std::lower_bound(
      std::begin(kMappings), std::end(kMappings), target,
      [](const Mapping &candidate, const std::uint32_t address) {
        return candidate.vram < address;
      });
  // Normalized overlay imports (including original CIC routines) must use
  // the same checked publication/lease path as other generated overlays.
  // The OS-execution profile never substitutes a mapped HLE overlay call.
  const bool mapped = mapping != std::end(kMappings) && mapping->vram == target &&
      !(state.guest_os_probe && target < kKseg0);
  const char *name = mapped ? mapping->name : nullptr;
  const std::size_t mapping_index =
      mapped ? static_cast<std::size_t>(mapping - std::begin(kMappings))
             : kMappingCount;
  if (name == nullptr) {
    if (recomp_func_t *const generated =
            jfg_generated_lookup_function(address)) {
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      // Generated calls do not preserve a meaningful guest return address, so
      // retain the host dispatch nesting explicitly. The shared hint-text
      // overlay is updated by a resident loop guarded by
      // runlinkIsModuleLoaded(). Its stable cross-overlay entry points can
      // otherwise execute while that guard still reports it as unloaded.
      const std::uint32_t caller_section = state.executing_generated_section;
      const bool caller_is_overlay = state.executing_generated_overlay;
      std::uint32_t target_section = UINT32_MAX;
      JfgGeneratedSectionMetadata target_metadata{};
      const auto cached_section = state.generated_section_cache.find(target);
      if (cached_section != state.generated_section_cache.end()) {
        target_section = cached_section->second.first;
        target_metadata = cached_section->second.second;
      } else {
        for (std::uint32_t section = 0U;
             section < jfg_generated_section_count(); ++section) {
          JfgGeneratedSectionMetadata metadata{};
          if (jfg_generated_section_metadata(section, &metadata) == 0)
            fail_closed_dispatch(state, "generated", "section-metadata",
                                 target);
          const std::uint64_t text_begin = metadata.linked_vram;
          const std::uint64_t text_end = text_begin + metadata.text_size;
          if (target < text_begin || target >= text_end)
            continue;
          target_section = section;
          target_metadata = metadata;
          break;
        }
        state.generated_section_cache.emplace(target,
            std::make_pair(target_section, target_metadata));
      }
      if (target_metadata.is_overlay == 1U &&
          !ensure_guest_overlay_allocation(state, target_metadata, *context))
        fail_closed_dispatch(state, "runlink", "guest-overlay-load", target);
      if (caller_is_overlay && target_metadata.is_overlay == 1U &&
          target_section == kHintTextOverlaySection &&
          target_section != caller_section) {
        const bool was_active =
            state.active_overlay_sections.contains(target_section);
        if (!publish_synthetic_overlay(state, target_section,
                                       target_metadata)) {
          fail_closed_overlay_publication(state, target);
        }
        if (!was_active)
          state.journal.append(NativeEventKind::kOverlayPublished,
                               target_section, target_metadata.linked_vram,
                               target);
      }
#endif
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      const bool trace_matrix_conversion = target == 0x80048D84U;
      const std::uint32_t matrix_source =
          static_cast<std::uint32_t>(context->r4);
      const std::uint32_t matrix_destination =
          static_cast<std::uint32_t>(context->r5);
      std::array<std::uint32_t, 4U> matrix_first_row{};
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      const bool trace_matrix_cat = target == 0x80048C7CU;
      const std::size_t matrix_cat_slot =
          state.recent_matrix_cat_position % state.recent_matrix_cat_args.size();
      if (trace_matrix_cat) {
        state.recent_matrix_cat_args[matrix_cat_slot] = {
            static_cast<std::uint32_t>(context->r4),
            static_cast<std::uint32_t>(context->r5),
            static_cast<std::uint32_t>(context->r6)};
        hle::GuestMemory memory(
            {rdram, kRdramSize},
            hle::GuestMemory::Layout::native_word_big_endian);
        for (std::size_t word = 0U; word < 4U; ++word)
          (void)memory.read_u32(
              static_cast<std::uint32_t>(context->r4) +
                  static_cast<std::uint32_t>(word * 4U),
              state.recent_matrix_cat_words[matrix_cat_slot][word]);
        for (std::size_t word = 0U; word < 4U; ++word)
          (void)memory.read_u32(
              static_cast<std::uint32_t>(context->r5) +
                  static_cast<std::uint32_t>(word * 16U),
              state.recent_matrix_cat_words[matrix_cat_slot][word + 4U]);
      }
#endif
      if (trace_matrix_conversion) {
        hle::GuestMemory memory(
            {rdram, kRdramSize},
            hle::GuestMemory::Layout::native_word_big_endian);
        for (std::size_t index = 0U; index < matrix_first_row.size(); ++index)
          (void)memory.read_u32(
              matrix_source + static_cast<std::uint32_t>(index * 4U),
              matrix_first_row[index]);
      }
#endif
      context->r0 = 0;
      if (state.generated_relocation_targets.contains(target)) {
        for (std::uint32_t section = 0U;
             section < jfg_generated_section_count(); ++section) {
          JfgGeneratedSectionMetadata metadata{};
          if (jfg_generated_section_metadata(section, &metadata) == 0)
            fail_closed_dispatch(state, "generated", "section-metadata",
                                 target);
          const std::uint64_t text_begin = metadata.linked_vram;
          const std::uint64_t text_end = text_begin + metadata.text_size;
          if (target < text_begin || target >= text_end)
            continue;
          for (std::uint64_t candidate = text_begin; candidate < target;
               candidate += sizeof(std::uint32_t)) {
            if (jfg_generated_lookup_function(
                    static_cast<std::int32_t>(candidate)) == generated) {
              context->r0 = target - metadata.linked_vram;
              break;
            }
          }
          break;
        }
      }
      ++state.generated_calls;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      state.executing_generated_section = target_section;
      state.executing_generated_overlay =
          target_metadata.is_overlay == 1U;
#endif
      [[maybe_unused]] const std::uint32_t allocation_size =
          static_cast<std::uint32_t>(context->r4);
#if defined(JFG_LIVE_RUNTIME)
      if (target == 0x80044FACU)
        trace_phase9_event(state, "update-begin", state.controller_samples);
      const bool focus_window = state.focus_update_start != 0U &&
          state.completed_game_updates + 1U >= state.focus_update_start - 1U &&
          state.completed_game_updates + 1U <= state.focus_update_end + 1U;
      const bool watch_active = state.watch_word_address != 0U &&
          focus_window;
      std::uint32_t watch_before = 0U;
      const std::uint32_t watch_return_address =
          static_cast<std::uint32_t>(context->r31);
      if (watch_active) {
        hle::GuestMemory watch_memory(
            {rdram, kRdramSize},
            hle::GuestMemory::Layout::native_word_big_endian);
        if (!watch_memory.read_u32(state.watch_word_address, watch_before))
          fail_closed_dispatch(state, "diagnostic", "watch-word-read", target);
        ++state.watch_dispatch_depth;
      }
      if (state.entry_probe_target == target && focus_window) {
        trace_guest_clock(state, "entry", target);
        if (++state.entry_probe_hits > 1024U)
          fail_closed_dispatch(state, "diagnostic", "entry-probe-overflow", target);
        state.entry_probe_trace << state.completed_game_updates + 1U << '\t'
            << state.vi_retraces << '\t' << state.controller_samples << '\t'
            << std::hex << "0x" << target << '\t' << "0x"
            << static_cast<std::uint32_t>(context->r4) << '\t' << "0x"
            << static_cast<std::uint32_t>(context->r5) << '\t' << "0x"
            << static_cast<std::uint32_t>(context->r6) << '\t' << "0x"
            << static_cast<std::uint32_t>(context->r7) << std::dec << '\n';
        state.entry_probe_trace.flush();
        if (state.entry_fpu_trace.is_open()) {
          state.entry_fpu_trace << state.completed_game_updates + 1U << '\t'
              << state.vi_retraces << '\t' << state.controller_samples << '\t'
              << std::hex << "0x" << target;
          const fpr *registers = &context->f0;
          for (std::size_t index = 0U; index < 32U; ++index) {
            state.entry_fpu_trace << '\t' << "0x" << std::setw(8)
                << std::setfill('0') << registers[index].u32l << '\t'
                << "0x" << std::setw(8) << registers[index].u32h;
          }
          state.entry_fpu_trace << std::setfill(' ') << std::dec << '\n';
          state.entry_fpu_trace.flush();
        }
        if (state.entry_gpr_trace.is_open()) {
          state.entry_gpr_trace << state.completed_game_updates + 1U << '\t'
              << state.vi_retraces << '\t' << state.controller_samples << '\t'
              << std::hex << "0x" << target;
          const gpr *registers = &context->r0;
          for (std::size_t index = 0U; index < 32U; ++index) {
            const auto value = static_cast<std::uint64_t>(registers[index]);
            state.entry_gpr_trace << '\t' << "0x" << std::setw(8)
                << std::setfill('0') << static_cast<std::uint32_t>(value)
                << '\t' << "0x" << std::setw(8)
                << static_cast<std::uint32_t>(value >> 32U);
          }
          state.entry_gpr_trace << std::setfill(' ') << std::dec << '\n';
          state.entry_gpr_trace.flush();
        }
        if (state.entry_memory_trace.is_open()) {
          hle::GuestMemory entry_memory(
              {rdram, kRdramSize},
              hle::GuestMemory::Layout::native_word_big_endian);
          state.entry_memory_trace << state.completed_game_updates + 1U << '\t'
              << state.vi_retraces << '\t' << state.controller_samples << '\t'
              << std::hex << "0x" << target;
          for (const std::uint64_t argument :
               {context->r5, context->r6, context->r7}) {
            const auto pointer_address = static_cast<std::uint32_t>(argument);
            if (pointer_address < 0x80000000U ||
                pointer_address > 0x803FFF00U)
              fail_closed_dispatch(state, "diagnostic", "entry-memory-range", target);
            state.entry_memory_trace << '\t';
            for (std::uint32_t offset = 0U; offset < 256U; offset += 4U) {
              std::uint32_t word = 0U;
              if (!entry_memory.read_u32(pointer_address + offset, word))
                fail_closed_dispatch(state, "diagnostic", "entry-memory-read", target);
              state.entry_memory_trace << std::setw(8) << std::setfill('0')
                  << word;
            }
          }
          state.entry_memory_trace << std::setfill(' ') << std::dec << '\n';
          state.entry_memory_trace.flush();
        }
      }
#endif
      [[maybe_unused]] const auto mod_actor_argument=static_cast<std::uint32_t>(context->r4);
      const auto sound_event_argument = static_cast<std::uint32_t>(context->r5);
#if defined(JFG_LIVE_RUNTIME)
      if (state.navigation_mod.enabled && target == 0x01002708U) {
        jfg::mod::Memory memory({rdram, kRdramSize});
        const auto original = static_cast<std::uint32_t>(context->r6);
        const auto boosted = state.navigation_mod.juno_run_target(
            memory, mod_actor_argument, sound_event_argument, original);
        context->r6 = boosted;
        if (boosted != original && state.navigation_mod.juno_run_boosts == 1U)
          jfg::support_event("mod=juno-run-speed-2x-applied");
      }
#endif
      generated(rdram, context);
      if (target == jfg::boot::kSoundNextEvent) {
        hle::GuestMemory memory({rdram, kRdramSize},
                               hle::GuestMemory::Layout::native_word_big_endian);
        if (jfg::boot::recover_empty_sound_queue(
                memory, *context, mod_actor_argument, sound_event_argument))
          ++state.sound_queue_recoveries;
      }
#if defined(JFG_LIVE_RUNTIME)
      if (state.navigation_mod.observing() && target==0x80032A48U) {
        jfg::mod::Memory memory({rdram,kRdramSize});
        (void)state.navigation_mod.observe_player(memory,mod_actor_argument);
        (void)state.navigation_mod.full_health(memory,mod_actor_argument);
      }
      if (target==0x80044FACU) update_navigation_mod(state);
#endif
#if defined(JFG_LIVE_RUNTIME)
      if (state.entry_probe_target == target && focus_window)
        trace_guest_clock(state, "entry-return", target);
      if (watch_active) {
        hle::GuestMemory watch_memory(
            {rdram, kRdramSize},
            hle::GuestMemory::Layout::native_word_big_endian);
        std::uint32_t watch_after = 0U;
        if (!watch_memory.read_u32(state.watch_word_address, watch_after))
          fail_closed_dispatch(state, "diagnostic", "watch-word-read", target);
        if (watch_before != watch_after) {
          if (++state.watch_dispatch_changes > 1024U)
            fail_closed_dispatch(state, "diagnostic", "watch-word-overflow", target);
          state.watch_word_trace << state.completed_game_updates + 1U << '\t'
              << state.vi_retraces << '\t' << state.controller_samples << '\t'
              << state.watch_dispatch_depth << '\t' << std::hex << "0x"
              << target << '\t' << "0x" << watch_return_address << '\t'
              << "0x" << watch_before << '\t' << "0x" << watch_after
              << std::dec << '\n';
          state.watch_word_trace.flush();
        }
        --state.watch_dispatch_depth;
      }
      if (target == 0x80044FACU) trace_gameplay_state(state, true);
      if (target == 0x80044FACU && state.update_hash_trace.is_open()) {
        ++state.completed_game_updates;
        trace_phase9_event(state, "update-end", state.controller_samples);
        write_retrace_semantic_hash(state, &state.update_hash_trace);
        if (state.update_word_trace.is_open()) {
          hle::GuestMemory word_memory(
              {rdram, kRdramSize},
              hle::GuestMemory::Layout::native_word_big_endian);
          std::uint32_t value = 0U;
          if (!word_memory.read_u32(state.update_word_address, value))
            fail_closed_dispatch(state, "diagnostic", "update-word-read", target);
          state.update_word_trace << state.completed_game_updates << '\t'
              << state.controller_samples << '\t' << state.vi_retraces
              << '\t' << state.vi_frames << '\t' << std::hex << "0x"
              << std::setw(8) << std::setfill('0') << state.update_word_address
              << '\t' << "0x" << std::setw(8) << value << std::setfill(' ')
              << std::dec << '\n';
        }
        if (state.focus_update_start != 0U &&
            state.completed_game_updates >= state.focus_update_start &&
            state.completed_game_updates <= state.focus_update_end) {
          const auto path = std::filesystem::path(state.retrace_hash_path).parent_path() /
              ("focus-update-" + std::to_string(state.completed_game_updates) + ".rdram");
          if (!write_logical_rdram_capture(state, path)) {
            std::fputs("native focus RDRAM capture failed\n", stderr);
            state.exit_requested = true;
          }
        }
        if (state.timing_trace.is_open())
          state.timing_trace << "game-update\t" << state.vi_frames << '\t'
                             << state.vi_retraces << "\t0\t"
                             << state.completed_game_updates << "\t0\t0\t0\n";
      }
      if (target == 0x8004A57CU && state.actor_timing_trace &&
          state.vi_retraces <= 100U && state.timing_trace.is_open()) {
        state.timing_trace << "mm-alloc\t" << state.vi_frames << '\t'
                           << state.vi_retraces << "\t0\t" << allocation_size
                           << '\t' << static_cast<std::uint32_t>(context->r2)
                           << "\t0\t0\n";
      }
#endif
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      state.executing_generated_section = caller_section;
      state.executing_generated_overlay = caller_is_overlay;
#endif
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (target == 0x80011C2CU && context->r2 != 0) // objCutCameraActive
        ++state.phase9_cutscene_active_calls;
      if (target == 0x00700D80U) {
        state.phase9_hinttext_active_last_return =
            static_cast<std::uint32_t>(context->r2);
        if (context->r2 == 0U)
          ++state.phase9_hinttext_active_zero_returns;
        else
          ++state.phase9_hinttext_active_nonzero_returns;
      }
#endif
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (trace_matrix_cat) {
        hle::GuestMemory memory(
            {rdram, kRdramSize},
            hle::GuestMemory::Layout::native_word_big_endian);
        (void)memory.read_u32(state.recent_matrix_cat_args[matrix_cat_slot][2],
                              state.recent_matrix_cat_words[matrix_cat_slot][8]);
        ++state.recent_matrix_cat_position;
      }
#endif
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (trace_matrix_conversion) {
        const std::size_t slot = state.recent_matrix_conversion_position %
            state.recent_matrix_conversion_sources.size();
        state.recent_matrix_conversion_sources[slot] = matrix_source;
        state.recent_matrix_conversion_destinations[slot] =
            matrix_destination;
        state.recent_matrix_conversion_first_rows[slot] = matrix_first_row;
        hle::GuestMemory memory(
            {rdram, kRdramSize},
            hle::GuestMemory::Layout::native_word_big_endian);
        (void)memory.read_u32(matrix_destination,
                              state.recent_matrix_conversion_output_words[slot][0]);
        (void)memory.read_u32(matrix_destination + 4U,
                              state.recent_matrix_conversion_output_words[slot][1]);
        (void)memory.read_u32(matrix_destination + 32U,
                              state.recent_matrix_conversion_output_words[slot][2]);
        (void)memory.read_u32(matrix_destination + 36U,
                              state.recent_matrix_conversion_output_words[slot][3]);
        if (matrix_source == 0x800FAAF0U) {
          std::size_t watch = 0U;
          while (watch < state.matrix_watch_addresses.size() &&
                 state.matrix_watch_addresses[watch] != 0U &&
                 state.matrix_watch_addresses[watch] != matrix_destination)
            ++watch;
          if (watch < state.matrix_watch_addresses.size()) {
            state.matrix_watch_addresses[watch] = matrix_destination;
            state.matrix_watch_words[watch] =
                state.recent_matrix_conversion_output_words[slot][0];
          }
        }
        ++state.recent_matrix_conversion_position;
      }
#endif
      return 1;
    }
    if (state.runtime != nullptr &&
        state.runtime->invoke_leased_function_by_guest_address(
            target, rdram, context) == jfg::GeneratedOverlayError::none)
      return 1;
    // The normalized generated ABI gives overlays stable synthetic call
    // addresses. A direct transfer to an exact generated overlay is therefore
    // its publication boundary in this native profile; arbitrary addresses
    // remain fail-closed.
    for (std::uint32_t section = 0U;
         section < jfg_generated_section_count(); ++section) {
      JfgGeneratedSectionMetadata metadata{};
      if (jfg_generated_section_metadata(section, &metadata) == 0)
        fail_closed_dispatch(state, "generated", "section-metadata",
                             target);
      const std::uint64_t text_begin = metadata.linked_vram;
      const std::uint64_t text_end = text_begin + metadata.text_size;
      if (metadata.is_overlay != 1U || target < text_begin || target >= text_end)
        continue;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (!ensure_guest_overlay_allocation(state, metadata, *context) ||
          !publish_synthetic_overlay(state, section, metadata)) {
#else
      if (jfg_generated_relocation_count(section) != 0U ||
          jfg_generated_section_lifecycle(
              1U, section, static_cast<std::int32_t>(metadata.linked_vram)) !=
              0) {
#endif
        fail_closed_overlay_publication(state, target);
      }
      state.journal.append(NativeEventKind::kOverlayPublished, section,
                           metadata.linked_vram, target);
      recomp_func_t *const published =
          jfg_generated_lookup_function(address);
      if (published == nullptr)
        fail_closed_dispatch(state, "generated", "overlay-lookup", target);
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      const std::uint32_t overlay_offset = target - metadata.linked_vram;
      if (section == 32U && overlay_offset == 0x2E8U) {
        note_hints_control_call(state, target, *context);
      } else if (section == 32U && overlay_offset == 0x2540U) {
        ++state.phase9_hints_talk_calls;
      }
#endif
      context->r0 = 0;
      if (state.generated_relocation_targets.contains(target)) {
        for (std::uint64_t candidate = text_begin; candidate < target;
             candidate += sizeof(std::uint32_t)) {
          if (jfg_generated_lookup_function(
                  static_cast<std::int32_t>(candidate)) == published) {
            context->r0 = target - metadata.linked_vram;
            break;
          }
        }
      }
      published(rdram, context);
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (target == 0x00700D80U) {
        state.phase9_hinttext_active_last_return =
            static_cast<std::uint32_t>(context->r2);
        if (context->r2 == 0U)
          ++state.phase9_hinttext_active_zero_returns;
        else
          ++state.phase9_hinttext_active_nonzero_returns;
      }
#endif
      return 1;
    }
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    // Runlink may preserve its own compact load base instead of the normalized
    // synthetic address. Translate calls through the live module table back to
    // the generated section identity and offset. Matching the full identity
    // prevents a nearby asset DMA from being mistaken for executable code.
    hle::GuestMemory runlink_memory(
        {rdram, kRdramSize},
        hle::GuestMemory::Layout::native_word_big_endian);
    RunlinkAddressResolution resolution{};
    const RunlinkAddressResult resolution_result =
        resolve_runlink_text_address(runlink_memory,
                                     kRunlinkModuleTablePointer,
                                     kRunlinkOverlaySlotCount, target,
                                     resolution);
    if (resolution_result == RunlinkAddressResult::resolved) {
      std::uint32_t matched_section = UINT32_MAX;
      JfgGeneratedSectionMetadata matched_metadata{};
      for (std::uint32_t section = 0U;
           section < jfg_generated_section_count(); ++section) {
        JfgGeneratedSectionMetadata metadata{};
        if (jfg_generated_section_metadata(section, &metadata) == 0)
          fail_closed_dispatch(state, "generated", "section-metadata",
                               target);
        if (metadata.is_overlay != 1U ||
            metadata.rom_start != resolution.identity.rom_start ||
            metadata.text_size != resolution.identity.text_size ||
            metadata.data_size != resolution.identity.data_size ||
            metadata.bss_size != resolution.identity.bss_size)
          continue;
        if (matched_section != UINT32_MAX)
          fail_closed_dispatch(state, "generated", "runlink-identity-duplicate",
                               target);
        matched_section = section;
        matched_metadata = metadata;
      }
      if (matched_section == UINT32_MAX ||
          resolution.text_offset >= matched_metadata.text_size ||
          resolution.text_offset > UINT32_MAX - matched_metadata.linked_vram)
        fail_closed_dispatch(state, "generated", "runlink-identity",
                             target);
      const std::uint32_t translated_target =
          matched_metadata.linked_vram + resolution.text_offset;
      if (!publish_synthetic_overlay(state, matched_section,
                                     matched_metadata)) {
        fail_closed_overlay_publication(state, target);
      }
      recomp_func_t *const published = jfg_generated_lookup_function(
          static_cast<std::int32_t>(translated_target));
      if (published == nullptr)
        fail_closed_dispatch(state, "generated", "runlink-overlay-lookup",
                             target);
      if (matched_section == 32U && resolution.text_offset == 0x2E8U) {
        note_hints_control_call(state, target, *context);
      } else if (matched_section == 32U &&
                 resolution.text_offset == 0x2540U) {
        ++state.phase9_hints_talk_calls;
      } else if (matched_section == 7U) {
        switch (resolution.text_offset) {
        case 0x148U:
          ++state.phase9_hinttext_start_calls;
          break;
        case 0xD6CU:
          ++state.phase9_hinttext_stop_calls;
          break;
        case 0x70CU:
          ++state.phase9_hinttext_advance_calls;
          break;
        case 0xD80U:
          ++state.phase9_hinttext_active_calls;
          break;
        case 0x1CCCU:
          ++state.phase9_hinttext_update_calls;
          break;
        case 0x16B4U:
          ++state.phase9_hinttext_accept_calls;
          break;
        default:
          break;
        }
      }
      state.journal.append(NativeEventKind::kOverlayPublished,
                           matched_section, matched_metadata.linked_vram,
                           target);
      context->r0 = 0;
      if (state.generated_relocation_targets.contains(translated_target)) {
        const std::uint64_t text_begin = matched_metadata.linked_vram;
        for (std::uint64_t candidate = text_begin;
             candidate < translated_target;
             candidate += sizeof(std::uint32_t)) {
          if (jfg_generated_lookup_function(
                  static_cast<std::int32_t>(candidate)) == published) {
            context->r0 = translated_target - matched_metadata.linked_vram;
            break;
          }
        }
      }
      ++state.generated_calls;
      published(rdram, context);
      if (matched_section == 7U && resolution.text_offset == 0xD80U) {
        state.phase9_hinttext_active_last_return =
            static_cast<std::uint32_t>(context->r2);
        if (context->r2 == 0U)
          ++state.phase9_hinttext_active_zero_returns;
        else
          ++state.phase9_hinttext_active_nonzero_returns;
      }
      return 1;
    }
    if (resolution_result == RunlinkAddressResult::memory_fault ||
        resolution_result == RunlinkAddressResult::duplicate_module ||
        resolution_result == RunlinkAddressResult::invalid_argument)
      fail_closed_dispatch(state, "generated", "runlink-address-table",
                           target);
#endif
    if (state.last_pi_size != 0U) {
      std::array<char, 96U> operation{};
      (void)std::snprintf(operation.data(), operation.size(),
                          "unresolved-dma-%08x-%08x-%08x",
                          state.last_pi_rom, state.last_pi_dram,
                          state.last_pi_size);
      fail_closed_dispatch(state, "generated", operation.data(), target);
    }
    fail_closed_dispatch(state, "generated", "unresolved", target);
  }
  ++state.mapped_calls;
  ++state.mapped_call_counts[mapping_index];
  hle::GuestMemory memory({rdram, kRdramSize},
                          hle::GuestMemory::Layout::native_word_big_endian);
  const auto a0 = static_cast<std::uint32_t>(context->r4),
             a1 = static_cast<std::uint32_t>(context->r5),
             a2 = static_cast<std::uint32_t>(context->r6),
             a3 = static_cast<std::uint32_t>(context->r7);
  if (state.guest_os_probe) {
    // No HLE CPU shortcuts in this profile: every mapped OS call must have
    // an original, instrumented body. Missing device behavior fails closed.
    recomp_func_t *const generated = jfg_generated_lookup_function(address);
    if (generated == nullptr)
      fail_closed_dispatch(state, "guest-os", "original-body-unavailable", target);
    // Input is latched at the same SDK poll boundary as the recorded route.
    // Original code still builds/reads every PIF packet and delivers messages.
    if (target == original_os_entry("osContStartReadData")) {
      ++state.controller_read_start_calls;
      sample_live_controller(state);
      state.mmio_trace->si_dma.sample(state.controller_ports.samples);
    }
    if (target == original_os_entry("osContGetReadData"))
      ++state.controller_get_data_calls;
    if (target == original_os_entry("osSpTaskLoad")) {
      if (state.mmio_trace->sp_deadline || state.mmio_trace->sp_launch ||
          !memory.read_u32(a0, state.loaded_rsp_task_type))
        fail_closed_dispatch(state, "guest-rsp", "task-load-state", target);
      state.loaded_rsp_task = a0;
      ++state.sp_task_load_calls;
    }
    if (target == original_os_entry("osSpTaskStartGo")) ++state.sp_task_start_calls;
    if (target == original_os_entry("osRecvMesg")) ++state.queue_recv_calls;
    if (target == original_os_entry("osSendMesg")) ++state.queue_send_calls;
    if (state.guest_clock_trace.is_open() && state.observed_guest_instructions < 1000000U)
      state.guest_clock_trace << "call\t" << std::hex << target << '\t' << a0 << '\t'
          << a1 << '\t' << a2 << std::dec << '\t' << state.cpu_count << '\n';
    generated(rdram, context);
    if (target == 0x80097520U) state.os_initialized = true;
#if defined(JFG_LIVE_RUNTIME)
    // RGO fast clock: a thread polling an empty queue (osRecvMesg NOBLOCK,
    // e.g. amWaitForMidiSync in cutscenes) waits for a device event, so skip
    // straight to it, as the idle loop does.
    if (target == original_os_entry("osRecvMesg") && state.fast_clock != 0U) {
      if (a2 == static_cast<std::uint32_t>(hle::kOsMesgNoBlock) &&
          static_cast<std::int32_t>(context->r2) == -1)
        skip_empty_poll(state, a0, static_cast<std::uint32_t>(context->r31));
    }
#endif
    if (target == original_os_entry("osContInit"))
      state.controller_initialized = static_cast<std::uint32_t>(context->r2) == 0U;
    // Read-only host observers. The original bodies above own all guest
    // register, queue and thread mutations, including interrupt delivery.
    if (target == original_os_entry("osCreateMesgQueue")) {
      state.queues.insert(a0);
      ++state.queue_create_calls;
    }
    if (target == original_os_entry("osSetEventMesg")) state.events[a0] = {a1, a2};
    if (target == original_os_entry("osViSetEvent")) {
      state.vi_queue = a0; state.vi_message = a1; state.vi_retrace_interval = a2;
    }
#if defined(JFG_LIVE_RUNTIME)
    if (target == original_os_entry("osViSetMode")) {
      if (!configure_live_vi_mode(state, memory, a0))
        fail_closed_dispatch(state, "guest-vi", "invalid-mode", target);
      state.vi_mode = a0;
    }
    if (target == original_os_entry("osViSetSpecialFeatures")) {
      state.vi_special_features = a0;
      state.rt64_vi.status = jfg::vi_apply_special_features(
          state.rt64_vi.status, state.vi_mode_control, a0);
    }
    if (target == original_os_entry("osViBlack")) {
      state.vi_blacked = a0 != 0;
      state.rt64_vi.horizontal_start = state.vi_blacked ? 0 : state.vi_mode_horizontal_start;
    }
#endif
    if (target == original_os_entry("osViSwapBuffer")) {
      ++state.vi_swap_calls;
      state.vi_current_framebuffer = state.vi_next_framebuffer;
      state.vi_next_framebuffer = a0;
    }
    if (target == original_os_entry("osSendMesg") && context->r2 == 0 &&
        a0 == state.vi_queue && a1 == state.vi_message) {
      ++state.vi_messages_delivered;
      present_completed_video(state,target);
    }
    if (target == original_os_entry("osRecvMesg") && context->r2 == 0 && a0 == state.vi_queue && a1 != 0) {
      std::uint32_t message = 0;
      if (!memory.read_u32(a1, message)) fail_closed_dispatch(state, "guest-vi", "message-unreadable", target);
      if (message == state.vi_message) {
        ++state.vi_retraces;
        jfg::support_breadcrumb(state.vi_frames, state.vi_retraces, state.controller_samples);
#if defined(JFG_LIVE_RUNTIME)
        write_retrace_semantic_hash(state);
        write_private_progress(state);
#endif
        if (state.vi_retraces >= state.retrace_target) retrace_gate(state);
      }
    }
    if (state.guest_clock_trace.is_open() && state.observed_guest_instructions < 1000000U) {
      state.guest_clock_trace << "return\t" << std::hex << target << std::dec << '\t'
          << state.cpu_count << '\n';
      state.guest_clock_trace.flush();
    }
    return 1;
  }
  if (state.guest_initializing) {
    // The independently executed cold-boot closure. Mapped private helpers
    // must execute their original bodies as well as the public OS entry.
    constexpr std::array<std::uint32_t, 15> helpers{
      0x80097788U,0x80098050U,0x800980d0U,0x80098360U,0x80099648U,
      0x80099748U,0x8009af40U,0x8009af50U,0x8009af60U,0x8009af70U,
      0x8009afc0U,0x8009b010U,0x8009b060U,0x8009b0c0U,0x8009cae0U};
    recomp_func_t *const generated = jfg_generated_lookup_function(address);
    if (!std::binary_search(helpers.begin(), helpers.end(), target) || generated == nullptr)
      fail_closed_dispatch(state, "guest-leaf", "initialize-closure", target);
    generated(rdram, context);
    return 1;
  }
  // These two leaf HLE calls account for most mapped dispatches in JFG. Use
  // their supported-ROM addresses before the descriptive name chain so normal
  // play does not repeat dozens of string comparisons for each call.
  if (target == 0x80096EA0U) {  // osVirtualToPhysical
    const std::uint32_t segment = a0 & 0xE0000000U;
    if (state.guest_leaf_probe) {
      // Execute the original supported-ROM routine, including its TLB
      // helper. The diagnostic CP0 bridge supplies explicit reference state;
      // unsupported operations still trap rather than bypassing the helper.
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr)
        fail_closed_dispatch(state, "guest-leaf", "unavailable", target);
      trace_guest_clock(state, "guest-leaf-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-leaf-return", target);
      if ((segment == 0x80000000U || segment == 0xA0000000U) &&
          static_cast<std::uint32_t>(context->r2) != (a0 & 0x1FFFFFFFU))
        fail_closed_dispatch(state, "guest-leaf", "translation-result", target);
      return 1;
    }
    const std::uint32_t physical =
        segment == 0x80000000U || segment == 0xA0000000U
            ? a0 & 0x1FFFFFFFU
            : a0;
    set_result(context, static_cast<std::int32_t>(physical));
    return 1;
  }
  if (target == 0x80098EA0U) {  // osSetIntMask
    if (state.guest_leaf_probe) {
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr || state.mmio_trace == nullptr ||
          !state.mmio_trace->guest_mi_mask || !state.mmio_trace->mi_mask.initialized())
        fail_closed_dispatch(state, "guest-leaf", "interrupt-mask-unavailable", target);
      trace_guest_clock(state, "guest-mask-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-mask-return", target);
      state.interrupt_mask =
          (static_cast<std::uint32_t>(cop0_status_read(context)) & 0xff01U) |
          (state.mmio_trace->mi_mask.read() << 16U);
      state.journal.append(NativeEventKind::kInterruptMask,
                           static_cast<std::uint32_t>(context->r2), state.interrupt_mask);
      return 1;
    }
    const std::uint32_t previous = state.interrupt_mask;
    state.interrupt_mask = a0;
    set_result(context, static_cast<std::int32_t>(previous));
    state.journal.append(NativeEventKind::kInterruptMask, previous, a0);
    return 1;
  }
  // osInitialize has no arguments or return value in the public libultra ABI.
  // The native runner has already established the memory image and generated
  // dispatch before guest entry, so its observable contract at this boundary
  // is to make initialization idempotent and allow execution to continue.
  if (std::strcmp(name, "osInitialize") == 0) {
    if (state.guest_leaf_probe) {
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (state.os_initialized || generated == nullptr || state.mmio_trace == nullptr ||
          !state.mmio_trace->guest_pif_boot)
        fail_closed_dispatch(state, "guest-leaf", "initialize-state", target);
      state.guest_initializing = true;
      generated(rdram, context);
      state.guest_initializing = false;
      if (!state.mmio_trace->pif_boot.acknowledged())
        fail_closed_dispatch(state, "guest-leaf", "initialize-pif", target);
    }
    state.os_initialized = true;
    return 1;
  }
  if (std::strcmp(name, "osCreateThread") == 0) {
    const std::uint32_t stack = static_cast<std::uint32_t>(context->r29);
    std::uint32_t guest_sp = 0U;
    std::uint32_t priority = 0U;
    if (!state.os_initialized || state.scheduler == nullptr ||
        !memory.read_u32(stack + 0x10U, guest_sp) ||
        !memory.read_u32(stack + 0x14U, priority) || a0 == 0U ||
        a2 == 0U || guest_sp < 16U || state.threads.contains(a0)) {
      fail_closed_dispatch(state, "hle", "malformed-thread", target);
    }
    recomp_func_t *const entry_function =
        jfg_generated_lookup_function(static_cast<std::int32_t>(a2));
    const bool synthetic_vi_manager =
        // The diagnostic inventory now contains this worker's original body.
        // Keep the cooperative control profile explicit, not dependent on a
        // missing symbol. The guest-OS scheduler will own its separate path.
        a2 == 0x80098bb8U && !state.vi_manager_thread_created &&
        state.events.contains(7U) && state.events.contains(3U);
    if (entry_function == nullptr && !synthetic_vi_manager) {
      fail_closed_dispatch(state, "generated", "thread-entry-unresolved", a2);
    }
    const std::uint32_t vi_internal_queue =
        synthetic_vi_manager ? state.events.at(7U).first : 0U;
    constexpr std::uint32_t kThreadStatus = 0x0400FF03U;
    const std::uint32_t thread_status =
        static_cast<std::int32_t>(a1) < 0
            ? kThreadStatus & ~0x04000000U
            : kThreadStatus;
    const int thread_id = state.scheduler->create_thread(
        priority, [&state, entry = a2, argument = a3,
                   guest_sp, entry_function,
                   vi_internal_queue,
                   thread_status, synthetic_vi_manager](ThreadScheduler &scheduler, int) {
          if (synthetic_vi_manager) {
            for (;;) {
              (void)scheduler.recv_blocking(vi_internal_queue, 0U);
              if (state.vi_queue == 0U ||
                  scheduler.send(state.vi_queue, state.vi_message, false) !=
                      hle::kOsSuccess) {
                fail_closed_dispatch(state, "vi", "retrace-delivery", entry);
              }
              ++state.vi_messages_delivered;
              state.journal.append(NativeEventKind::kViMessageDelivered,
                                   state.vi_queue, state.vi_message,
                                   static_cast<std::uint32_t>(
                                       state.vi_messages_delivered));
            }
          }
          recomp_context thread_context{};
          thread_context.r4 = sign_extend_mips32(argument);
          thread_context.r29 = sign_extend_mips32(guest_sp - 16U);
          // osCreateThread stores an exception-return context, not the live
          // entry Status. The native handoff replaces __osDispatchThread's
          // ERET, so apply its Status transition before running guest code.
          cop0_status_write(&thread_context, status_after_exception_return(thread_status));
#if defined(JFG_PHASE8_LIVE_RUNTIME)
          if (state.guest_leaf_probe && state.timing_trace.is_open())
            state.timing_trace << "thread-entry-status\t" << std::hex << entry << '\t'
                               << cop0_status_read(&thread_context) << '\t'
                               << state.mmio_trace->mi_mask.read() << std::dec << '\n';
#endif
          entry_function(state.rdram, &thread_context);
        });
    state.vi_manager_thread_created |= synthetic_vi_manager;
    if (state.guest_leaf_probe) {
      if (static_cast<std::size_t>(thread_id) != state.saved_rcp_masks.size())
        fail_closed_dispatch(state, "guest-leaf", "thread-mask-index", target);
      // Independent original-OS thread-entry probe: all six sources enabled.
      state.saved_rcp_masks.push_back(0x3fU);
    }
    state.threads.emplace(a0, thread_id);
    state.thread_order.emplace_back(a0, thread_id);
    state.thread_entries.push_back(a2);
    state.journal.append(NativeEventKind::kThreadCreated, a0, a2, priority);
    return 1;
  }
  if (std::strcmp(name, "osStartThread") == 0) {
    const auto thread = state.threads.find(a0);
    if (state.scheduler == nullptr || thread == state.threads.end()) {
      fail_closed_dispatch(state, "hle", "unknown-thread", target);
    }
    state.scheduler->start_thread(thread->second);
    state.journal.append(NativeEventKind::kThreadStarted, a0,
                         static_cast<std::uint32_t>(thread->second));
    if (state.scheduler_running) {
      state.scheduler->yield_current();
    } else {
      state.scheduler_running = true;
      auto outcome = state.scheduler->run(1'000'000U);
      while ((outcome == ScheduleOutcome::kQuiescent ||
              outcome == ScheduleOutcome::kTimeslice) &&
             state.vi_retraces < state.retrace_target &&
             state.vi_retrace_interval != 0U &&
             state.vi_internal_queue != 0U) {
#if defined(JFG_LIVE_RUNTIME)
        if (state.focus_update_start != 0U &&
            state.completed_game_updates + 1U >= state.focus_update_start - 1U &&
            state.completed_game_updates + 1U <= state.focus_update_end + 1U &&
            state.timing_trace.is_open()) {
          std::string blocked_threads;
          for (const auto &[guest, id] : state.thread_order) {
            (void)guest;
            const auto snapshot = state.scheduler->snapshot_of(id);
            if (!snapshot.has_value() ||
                snapshot->state != ThreadState::kBlocked)
              continue;
            if (!blocked_threads.empty())
              blocked_threads.push_back(',');
            char queue[11U]{};
            (void)std::snprintf(queue, sizeof(queue), "%08x",
                                snapshot->blocked_queue);
            blocked_threads += std::to_string(id) + "@" + queue;
          }
          std::vector<std::pair<std::uint32_t, std::uint32_t>> mapped_events;
          mapped_events.reserve(state.events.size());
          for (const auto &[event, queue_and_message] : state.events) {
            mapped_events.emplace_back(event, queue_and_message.first);
          }
          std::sort(mapped_events.begin(), mapped_events.end());
          std::string event_queues;
          for (const auto &[event, queue_address] : mapped_events) {
            if (!event_queues.empty())
              event_queues.push_back(',');
            char queue[11U]{};
            (void)std::snprintf(queue, sizeof(queue), "%08x",
                                queue_address);
            event_queues += std::to_string(event) + "@" + queue;
          }
          state.timing_trace << "vi-service\t" << state.vi_frames << '\t'
                             << state.vi_retraces << "\t0\t"
                             << (outcome == ScheduleOutcome::kTimeslice
                                     ? "timeslice"
                                     : "quiescent")
                             << '\t' << state.completed_game_updates << '\t'
                             << state.interrupt_timeslices << '\t'
                             << state.pending_graphics_tasks.size() << ':'
                             << state.pending_si_completions << ':'
                             << state.scheduler->dispatch_count() << ':'
                             << blocked_threads << ':' << event_queues << '\n';
        }
#endif
        service_vi_frame(state, target);
        outcome = state.scheduler->run(1'000'000U);
      }
      state.scheduler_running = false;
      if (outcome == ScheduleOutcome::kUnbounded)
        fail_closed_dispatch(state, "scheduler", "step-budget", target);
    }
    return 1;
  }
  if (std::strcmp(name, "osSetThreadPri") == 0) {
    if (a0 == 0U) {
      const int current = state.scheduler == nullptr
                              ? -1
                              : state.scheduler->current_thread_id();
      if (state.scheduler_running && current >= 0) {
        if (!state.scheduler->set_priority(current, a1)) {
          fail_closed_dispatch(state, "hle", "unknown-thread", target);
        }
        state.scheduler->yield_current();
      } else {
        state.root_priority = a1;
      }
      return 1;
    }
    const auto thread = state.threads.find(a0);
    if (state.scheduler == nullptr || thread == state.threads.end() ||
        !state.scheduler->set_priority(thread->second, a1)) {
      fail_closed_dispatch(state, "hle", "unknown-thread", target);
    }
    return 1;
  }
  if (std::strcmp(name, "osGetThreadPri") == 0) {
    if (a0 == 0U) {
      const int current = state.scheduler == nullptr
                              ? -1
                              : state.scheduler->current_thread_id();
      const auto priority =
          state.scheduler_running && current >= 0
              ? state.scheduler->priority_of(current)
              : std::optional<std::uint32_t>{state.root_priority};
      set_result(context, static_cast<std::int32_t>(*priority));
      return 1;
    }
    const auto thread = state.threads.find(a0);
    const auto priority =
        thread == state.threads.end() || state.scheduler == nullptr
            ? std::optional<std::uint32_t>{}
            : state.scheduler->priority_of(thread->second);
    if (!priority.has_value()) {
      fail_closed_dispatch(state, "hle", "unknown-thread", target);
    }
    set_result(context, static_cast<std::int32_t>(*priority));
    return 1;
  }
  if (std::strcmp(name, "osSetEventMesg") == 0) {
    if (!state.queues.contains(a1)) {
      fail_closed_dispatch(state, "hle", "unknown-queue", target);
    }
    state.events[a0] = {a1, a2};
    if (a0 == 7U) {
      state.vi_internal_queue = a1;
      state.vi_internal_message = a2;
    }
    return 1;
  }
  if (std::strcmp(name, "osCreatePiManager") == 0) {
    if (state.pi_command_queue != 0U) {
      set_result(context, static_cast<std::int32_t>(state.pi_command_queue));
      return 1;
    }
    if (state.scheduler == nullptr || a1 == 0U || a2 == 0U || a3 == 0U ||
        !state.scheduler->create_queue(a1, a2,
                                       static_cast<std::int32_t>(a3))) {
      fail_closed_dispatch(state, "hle", "pi-manager-setup", target);
    }
    state.queues.insert(a1);
    state.pi_command_queue = a1;
    state.journal.append(NativeEventKind::kQueueCreated, a1, a2, a3);
    set_result(context, static_cast<std::int32_t>(a1));
    return 1;
  }
  if (std::strcmp(name, "osPiGetCmdQueue") == 0) {
    if (state.pi_command_queue == 0U)
      fail_closed_dispatch(state, "hle", "pi-manager-uninitialized", target);
    set_result(context,
               static_cast<std::int32_t>(state.pi_command_queue));
    return 1;
  }
  if (std::strcmp(name, "osPiStartDma") == 0) {
    const std::uint32_t stack = static_cast<std::uint32_t>(context->r29);
    std::uint32_t dram_address = 0U;
    std::uint32_t size = 0U;
    std::uint32_t completion_queue = 0U;
    if (state.rom == nullptr || state.scheduler == nullptr || a0 == 0U ||
        a2 != 0U || !memory.read_u32(stack + 0x10U, dram_address) ||
        !memory.read_u32(stack + 0x14U, size) ||
        !memory.read_u32(stack + 0x18U, completion_queue) || size == 0U ||
        !state.queues.contains(completion_queue)) {
      fail_closed_dispatch(state, "hle", "invalid-pi-dma", target);
    }
    const std::uint32_t segment = dram_address & 0xE0000000U;
    const std::uint64_t dram_offset = dram_address & 0x1FFFFFFFU;
    const std::uint64_t rom_offset =
        a3 >= 0x10000000U ? std::uint64_t{a3} - 0x10000000U : a3;
    if ((segment != 0x80000000U && segment != 0xA0000000U) ||
        dram_offset > kRdramSize || size > kRdramSize - dram_offset ||
        rom_offset > state.rom_size || size > state.rom_size - rom_offset) {
      fail_closed_dispatch(state, "hle", "pi-dma-range", target);
    }
    for (std::uint32_t index = 0U; index < size; ++index) {
      state.rdram[(static_cast<std::size_t>(dram_offset) + index) ^ 3U] =
          state.rom[static_cast<std::size_t>(rom_offset) + index];
    }
    for (std::uint32_t section = 0U;
         section < jfg_generated_section_count(); ++section) {
      JfgGeneratedSectionMetadata metadata{};
      if (jfg_generated_section_metadata(section, &metadata) == 0 ||
          metadata.is_overlay != 1U || metadata.rom_start != rom_offset)
        continue;
      // A runlink module may load text and data with separate PI requests.
      // Matching the module's exact ROM start is the publication boundary;
      // publish_synthetic_overlay independently validates and materializes the
      // complete normalized image from the supported ROM.
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      SyntheticOverlayPublication publication =
          SyntheticOverlayPublication::ensure_active;
      if (section == 6U &&
          state.phase9_death_restart_calls >
              state.health_overlay_death_reload_generation &&
          state.active_overlay_sections.contains(section)) {
        // The same ROM range can be read for reasons other than loading its
        // overlay. Reload mutable data/BSS only when the DMA destination is
        // the base of the matching live runlink module. Treating every ROM
        // read as an unload/reload resets instrument state continuously and
        // can prevent the scene framebuffer from ever becoming presentable.
        RunlinkAddressResolution resolution{};
        const std::uint32_t dma_guest_address =
            kKseg0 | static_cast<std::uint32_t>(dram_offset);
        if (resolve_runlink_text_address(
                memory, kRunlinkModuleTablePointer,
                kRunlinkOverlaySlotCount, dma_guest_address,
                resolution) == RunlinkAddressResult::resolved &&
            resolution.text_offset == 0U &&
            resolution.identity.rom_start == metadata.rom_start &&
            resolution.identity.text_size == metadata.text_size &&
            resolution.identity.data_size == metadata.data_size &&
            resolution.identity.bss_size == metadata.bss_size)
          publication = SyntheticOverlayPublication::reload;
      }
#endif
      if (
#if defined(JFG_PHASE8_LIVE_RUNTIME)
          !publish_synthetic_overlay(state, section, metadata, publication)
#else
          jfg_generated_relocation_count(section) != 0U ||
          jfg_generated_section_lifecycle(
              1U, section, static_cast<std::int32_t>(metadata.linked_vram)) !=
              0
#endif
      ) {
        fail_closed_overlay_publication(state, metadata.linked_vram);
      }
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (publication == SyntheticOverlayPublication::reload)
        state.health_overlay_death_reload_generation =
            state.phase9_death_restart_calls;
#endif
      state.journal.append(NativeEventKind::kOverlayPublished, section,
                           metadata.linked_vram,
                           static_cast<std::uint32_t>(dram_offset));
      break;
    }
    if (!memory.write_u32(a0 + 0x00U, completion_queue) ||
        !memory.write_u32(a0 + 0x04U, a1) ||
        !memory.write_u32(a0 + 0x08U, 0U) ||
        !memory.write_u32(a0 + 0x0CU, dram_address) ||
        !memory.write_u32(a0 + 0x10U, a3) ||
        !memory.write_u32(a0 + 0x14U, size) ||
        state.scheduler->send(completion_queue, a0, false) !=
            hle::kOsSuccess) {
      fail_closed_dispatch(state, "hle", "pi-dma-completion", target);
    }
    state.journal.append(NativeEventKind::kPiDma, a3, dram_address, size);
    state.last_pi_rom = a3;
    state.last_pi_dram = dram_address;
    state.last_pi_size = size;
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osCic6105SendData") == 0 ||
      std::strcmp(name, "osCic6105StartGetData") == 0) {
    recomp_func_t *const generated =
        std::strcmp(name, "osCic6105SendData") == 0 ? fn_044_0000
                                                     : fn_044_0001;
    generated(rdram, context);
    return 1;
  }
  if (std::strcmp(name, "__osSiRawStartDma") == 0) {
    if (state.si_count_probe && state.si_deadline.next()) {
      set_result(context, -1);
      return 1;
    }
    recomp_func_t *const generated = jfg_generated_lookup_function(address);
    if (generated == nullptr)
      fail_closed_dispatch(state, "si", "raw-dma-unavailable", target);
    generated(rdram, context);
    if (static_cast<std::uint32_t>(context->r2) == 0U) {
      if (a0 > 1U)
        fail_closed_dispatch(state, "si", "raw-dma-direction", target);
      if (a0 == 1U) {
        std::array<std::uint8_t, jfg::kPifRamBytes> transfer{};
        for (std::size_t index = 0U; index < transfer.size(); ++index) {
          std::size_t guest_offset = 0U;
          if (!guest_byte_offset(
                  a1 + static_cast<std::uint32_t>(index), guest_offset)) {
            fail_closed_dispatch(state, "si", "raw-dma-range", target);
          }
          transfer[index] = state.rdram[guest_offset];
        }
        // The high-level controller path owns ordinary PIF channel traffic.
        // Retain only a real CIC challenge here; mirroring unimplemented
        // controller commands back into RDRAM would overwrite the controller
        // data produced by that HLE path.
        if (jfg::process_cic_nus_6105_challenge(transfer) ||
            (state.controller_guest_init_probe &&
             jfg::process_controller_status_query(transfer, state.controller_ports.mask()))) {
          state.pif_ram = transfer;
          state.cic_response_pending = true;
        }
      } else if (state.cic_response_pending) {
        for (std::size_t index = 0U; index < state.pif_ram.size(); ++index) {
          std::size_t guest_offset = 0U;
          if (!guest_byte_offset(
                  a1 + static_cast<std::uint32_t>(index), guest_offset)) {
            fail_closed_dispatch(state, "si", "raw-dma-range", target);
          }
          state.rdram[guest_offset] = state.pif_ram[index];
        }
        state.cic_response_pending = false;
      }
      if (state.pending_si_completions ==
          (std::numeric_limits<std::uint32_t>::max)())
        fail_closed_dispatch(state, "si", "completion-overflow", target);
      ++state.pending_si_completions;
      if (state.si_count_probe) schedule_si_deadline(state, target);
    }
    return 1;
  }
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  if (std::strcmp(name, "osSetTimer") == 0) {
    const auto sp = static_cast<std::uint32_t>(context->r29);
    std::uint32_t interval_hi = 0, interval_lo = 0, queue = 0, message = 0;
    if (sp > UINT32_MAX - 0x1CU ||
        !memory.read_u32(sp + 0x10U, interval_hi) ||
        !memory.read_u32(sp + 0x14U, interval_lo) ||
        !memory.read_u32(sp + 0x18U, queue) ||
        !memory.read_u32(sp + 0x1CU, message) ||
        !state.timers.set(memory, state.cpu_count, a0,
                         (std::uint64_t{a2} << 32U) | a3,
                         (std::uint64_t{interval_hi} << 32U) | interval_lo,
                         queue, message))
      fail_closed_dispatch(state, "timer", "invalid-timer-arguments", target);
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osStopTimer") == 0) {
    state.timers.stop(a0);
    return 1;
  }
  if (std::strcmp(name, "osGetCount") == 0) {
    set_result(context, static_cast<std::int32_t>(state.cpu_count));
    return 1;
  }
  if (std::strcmp(name, "osSetTime") == 0) {
    state.os_time_base = (std::uint64_t{a0} << 32U) | a1;
    state.os_time_count_origin = state.cpu_count;
    return 1;
  }
  if (std::strcmp(name, "osGetTime") == 0) {
    const std::uint64_t value =
        state.os_time_base + (state.cpu_count - state.os_time_count_origin);
    context->r2 = sign_extend_mips32(static_cast<std::uint32_t>(value >> 32U));
    context->r3 = sign_extend_mips32(static_cast<std::uint32_t>(value));
    return 1;
  }
  if (std::strcmp(name, "osFlashInit") == 0 ||
      std::strcmp(name, "osFlashReInit") == 0) {
    set_result(context, static_cast<std::int32_t>(0x80104EE0U));
    return 1;
  }
  if (std::strcmp(name, "osFlashReadStatus") == 0) {
    if (!write_guest_byte(state, a0, 0U))
      fail_closed_dispatch(state, "flashram", "status-output", target);
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osFlashReadId") == 0) {
    if (!memory.write_u32(a0, 0x11118001U) ||
        !memory.write_u32(a1, 0x00C2001EU))
      fail_closed_dispatch(state, "flashram", "id-output", target);
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osFlashClearStatus") == 0) {
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osFlashAllErase") == 0) {
    if (!state.flashram.begin_erase_all().ok() ||
        !complete_flash_operation(state)) {
      set_result(context, -1);
      return 1;
    }
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osFlashSectorErase") == 0) {
    if (!state.flashram.begin_erase_page(a0).ok() ||
        !complete_flash_operation(state)) {
      set_result(context, -1);
      return 1;
    }
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osFlashWriteBuffer") == 0) {
    std::array<std::uint8_t, jfg::kFlashRamPageBytes> page{};
    const std::uint32_t physical = a2 & 0x1FFFFFFFU;
    if (state.scheduler == nullptr || !state.queues.contains(a3) ||
        physical > kRdramSize || page.size() > kRdramSize - physical)
      fail_closed_dispatch(state, "flashram", "write-buffer-arguments", target);
    for (std::size_t index = 0U; index < page.size(); ++index)
      page[index] = state.rdram[(physical + index) ^ 3U];
    if (!state.flashram.stage_write(page).ok() ||
        state.scheduler->send(a3, 0U, false) != hle::kOsSuccess)
      fail_closed_dispatch(state, "flashram", "write-buffer-completion", target);
    set_result(context, hle::kOsSuccess);
    if (state.scheduler_running)
      state.scheduler->yield_current();
    return 1;
  }
  if (std::strcmp(name, "osFlashWriteArray") == 0) {
    if (!state.flashram.begin_program(a0).ok() ||
        !complete_flash_operation(state)) {
      set_result(context, -1);
      return 1;
    }
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osFlashReadArray") == 0) {
    const std::uint32_t stack = static_cast<std::uint32_t>(context->r29);
    std::uint32_t page_count = 0U, completion_queue = 0U;
    const std::uint64_t flash_offset =
        std::uint64_t{a2} * jfg::kFlashRamPageBytes;
    const std::uint32_t physical = a3 & 0x1FFFFFFFU;
    if (state.scheduler == nullptr ||
        !memory.read_u32(stack + 0x10U, page_count) ||
        !memory.read_u32(stack + 0x14U, completion_queue) || page_count == 0U ||
        !state.queues.contains(completion_queue))
      fail_closed_dispatch(state, "flashram", "read-array-arguments", target);
    const std::uint64_t byte_count =
        std::uint64_t{page_count} * jfg::kFlashRamPageBytes;
    if (flash_offset > jfg::kFlashRamSizeBytes ||
        byte_count > jfg::kFlashRamSizeBytes - flash_offset ||
        physical > kRdramSize || byte_count > kRdramSize - physical)
      fail_closed_dispatch(state, "flashram", "read-array-range", target);
    const auto read = state.flashram.read(static_cast<std::size_t>(flash_offset),
                                          static_cast<std::size_t>(byte_count));
    if (!read.ok())
      fail_closed_dispatch(state, "flashram", "read-array-device", target);
    for (std::size_t index = 0U; index < read.bytes.size(); ++index)
      state.rdram[(physical + index) ^ 3U] = read.bytes[index];
    if (state.scheduler->send(completion_queue, 0U, false) != hle::kOsSuccess)
      fail_closed_dispatch(state, "flashram", "read-array-completion", target);
    set_result(context, hle::kOsSuccess);
    if (state.scheduler_running)
      state.scheduler->yield_current();
    return 1;
  }
#endif
  if (std::strcmp(name, "osContInit") == 0) {
#if defined(_WIN32)
    char probe_value[2]{};
    const bool guest_init_probe = GetEnvironmentVariableA(
        "JFG_PHASE9_CONTROLLER_GUEST_INIT", probe_value, sizeof(probe_value)) == 1 &&
        probe_value[0] == '1';
#else
    const char *probe_value = std::getenv("JFG_PHASE9_CONTROLLER_GUEST_INIT");
    const bool guest_init_probe = probe_value != nullptr &&
                                  std::strcmp(probe_value, "1") == 0;
#endif
    if (guest_init_probe) {
#if defined(_WIN32)
      char si_probe_value[2]{};
      state.si_count_probe = GetEnvironmentVariableA("JFG_PHASE9_SI_COUNT_PROBE",
          si_probe_value, sizeof(si_probe_value)) == 1 && si_probe_value[0] == '1';
#else
      const char *si_probe_value = std::getenv("JFG_PHASE9_SI_COUNT_PROBE");
      state.si_count_probe = si_probe_value != nullptr && std::strcmp(si_probe_value, "1") == 0;
#endif
      // Bounded diagnostic candidate: original code establishes the guest
      // channel count, command state, access queues, and initialization guard.
      state.controller_guest_init_probe = true;
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr)
        fail_closed_dispatch(state, "si", "controller-init-unavailable", target);
      generated(rdram, context);
      state.controller_initialized = static_cast<std::uint32_t>(context->r2) == 0U;
      return 1;
    }
#if defined(JFG_LIVE_RUNTIME)
    sample_controller_ports(state);
    const auto connected_mask=state.input_replay_loaded ? std::uint8_t{1} : state.controller_ports.mask();
#else
    const std::uint8_t connected_mask=1;
#endif
    if (state.scheduler == nullptr || !state.queues.contains(a0) || a1 == 0U ||
        a2 == 0U || !write_guest_byte(state, a1, connected_mask)) {
      fail_closed_dispatch(state, "hle", "controller-init", target);
    }
    for (std::uint32_t controller = 0U; controller < 4U; ++controller) {
      const std::uint32_t status = a2 + controller * 4U;
      const bool connected = (connected_mask & (1U<<controller))!=0;
      if (!write_guest_byte(state, status, 0U) ||
          !write_guest_byte(state, status + 1U, connected ? 5U : 0U) ||
          !write_guest_byte(state, status + 2U, 0U) ||
          !write_guest_byte(state, status + 3U, connected ? 0U : 8U)) {
        fail_closed_dispatch(state, "hle", "controller-status", target);
      }
    }
    state.controller_initialized = true;
    state.journal.append(NativeEventKind::kController, 1U, a0);
    set_result(context, hle::kOsSuccess);
    return 1;
  }
  if (std::strcmp(name, "osContStartReadData") == 0) {
    if (state.si_count_probe && state.si_deadline.next()) {
      set_result(context, -1);
      return 1;
    }
    ++state.controller_read_start_calls;
#if defined(JFG_LIVE_RUNTIME)
    trace_phase9_event(state, "start-entry", state.controller_samples);
#endif
    state.controller_read_thread = state.scheduler == nullptr
                                       ? -1
                                       : state.scheduler->current_thread_id();
    const auto si = state.events.find(5U);
    if (!state.controller_initialized || state.scheduler == nullptr ||
        !state.queues.contains(a0) || si == state.events.end() ||
        si->second.first != a0 ||
        state.pending_si_completions ==
            (std::numeric_limits<std::uint32_t>::max)()) {
      fail_closed_dispatch(state, "hle", "controller-read-start", target);
    }
    ++state.pending_si_completions;
    if (state.si_count_probe) schedule_si_deadline(state, target);
#if defined(JFG_LIVE_RUNTIME)
    trace_si_timing(state, "read-start", a0, 0U, 0);
    if (state.play_mode || state.input_replay_loaded) {
      std::size_t pressed_high = 0U, pressed_low = 0U;
      std::size_t current_stick_x = 0U, current_stick_y = 0U;
      std::size_t menu_stick_x = 0U, menu_stick_y = 0U;
      std::uint16_t game_pressed = 0U;
      if (!guest_byte_offset(0x800FB0F0U, pressed_high) ||
          !guest_byte_offset(0x800FB0F1U, pressed_low) ||
          !guest_byte_offset(0x800FB0C2U, current_stick_x) ||
          !guest_byte_offset(0x800FB0C3U, current_stick_y) ||
          !guest_byte_offset(0x800FF3D8U, menu_stick_x) ||
          !guest_byte_offset(0x800FF3DCU, menu_stick_y))
        fail_closed_dispatch(state, "controller", "pressed-observation",
                             target);
      game_pressed = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(state.rdram[pressed_high]) << 8U) |
          state.rdram[pressed_low]);
      if (game_pressed != 0U) {
        ++state.game_pressed_observations;
        state.last_game_pressed_buttons = game_pressed;
      }
      const auto game_stick_x =
          static_cast<std::int8_t>(state.rdram[current_stick_x]);
      const auto game_stick_y =
          static_cast<std::int8_t>(state.rdram[current_stick_y]);
      if (game_stick_x != 0 || game_stick_y != 0) {
        ++state.game_stick_observations;
        state.last_game_stick_x = game_stick_x;
        state.last_game_stick_y = game_stick_y;
      }
      const auto observed_menu_stick_x =
          static_cast<std::int8_t>(state.rdram[menu_stick_x]);
      const auto observed_menu_stick_y =
          static_cast<std::int8_t>(state.rdram[menu_stick_y]);
      if (observed_menu_stick_x != 0 || observed_menu_stick_y != 0) {
        ++state.menu_stick_observations;
        state.last_menu_stick_x = observed_menu_stick_x;
        state.last_menu_stick_y = observed_menu_stick_y;
      }
      sample_live_controller(state);
      if (state.poll_hash_trace.is_open())
        write_retrace_semantic_hash(state, nullptr, &state.poll_hash_trace);
      trace_phase9_event(state, "start-exit", state.controller_samples - 1U);
    }
#endif
    state.journal.append(NativeEventKind::kController, 2U, a0,
                         si->second.second);
    set_result(context, hle::kOsSuccess);
#if defined(JFG_LIVE_RUNTIME)
    // Stop after the Nth controller-read HLE is complete, before the guest
    // consumes its result. This is an input-poll boundary, not a VI boundary.
    if (state.poll_target != 0U &&
        state.controller_samples == state.poll_target)
      probe_target_gate(state);
#endif
    return 1;
  }
  if (std::strcmp(name, "osContGetReadData") == 0) {
    ++state.controller_get_data_calls;
#if defined(JFG_LIVE_RUNTIME)
    trace_phase9_event(state, "get-entry", state.controller_samples == 0U
                                             ? 0U : state.controller_samples - 1U);
#endif
    if (!state.controller_initialized || a0 == 0U) {
      fail_closed_dispatch(state, "hle", "controller-read-data", target);
    }
    for (std::uint32_t controller = 0U; controller < 4U; ++controller) {
      for (std::uint32_t byte = 0U; byte < 6U; ++byte) {
        std::uint8_t value = byte == 4U && controller != 0U ? 8U : 0U;
#if defined(JFG_LIVE_RUNTIME)
        if (state.play_mode || state.input_replay_loaded) {
          const auto& port=state.controller_ports.samples[controller];
          if (!port.connected) {
            value = byte == 4U ? 8U : 0U;
          } else {
          switch (byte) {
          case 0U:
            value = static_cast<std::uint8_t>(
                port.buttons >> 8U);
            break;
          case 1U:
            value = static_cast<std::uint8_t>(
                port.buttons);
            break;
          case 2U:
            value = static_cast<std::uint8_t>(
                port.x);
            break;
          case 3U:
            value = static_cast<std::uint8_t>(
                port.y);
            break;
          default:
            value = 0U;
            break;
          }
          if (byte == 0U && port.buttons != 0U)
            ++state.non_neutral_controller_writes;
          }
        }
#endif
        if (!write_guest_byte(state, a0 + controller * 6U + byte, value)) {
          fail_closed_dispatch(state, "hle", "controller-pad-range", target);
        }
      }
    }
    state.journal.append(NativeEventKind::kController, 3U, a0);
#if defined(JFG_LIVE_RUNTIME)
    trace_phase9_event(state, "get-exit", state.controller_samples == 0U
                                            ? 0U : state.controller_samples - 1U);
    if (state.controller_return_trace.is_open()) {
      const std::uint64_t poll = state.controller_samples == 0U
                                     ? 0U : state.controller_samples - 1U;
      bool selected = false;
      for (std::size_t index = 0U; index < state.event_range_count; ++index) {
        const auto [first, last] = state.event_ranges[index];
        selected |= poll >= first && poll <= last;
      }
      if (selected) {
        if (++state.controller_return_rows > 256U)
          fail_closed_dispatch(state, "diagnostic", "controller-return-overflow",
                               target);
        state.controller_return_trace << state.controller_return_rows << '\t'
            << poll << '\t' << state.completed_game_updates << '\t'
            << state.vi_retraces << '\t' << state.vi_frames << '\t'
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << a0
            << '\t';
        for (std::uint32_t byte = 0U; byte < 24U; ++byte) {
          std::size_t offset = 0U;
          if (!guest_byte_offset(a0 + byte, offset))
            fail_closed_dispatch(state, "diagnostic", "controller-return-range",
                                 target);
          state.controller_return_trace << std::setw(2)
                                        << static_cast<unsigned>(state.rdram[offset]);
        }
        state.controller_return_trace << std::setfill(' ') << std::dec << '\n';
      }
    }
#endif
    return 1;
  }
  if (std::strcmp(name, "bzero") == 0) {
    const auto length = static_cast<std::int32_t>(a1);
    const auto guest_bzero = [&]() {
      if (static_cast<std::int64_t>(context->r5) != static_cast<std::int64_t>(length))
        fail_closed_dispatch(state, "guest-leaf", "bzero-length-extension", target);
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr)
        fail_closed_dispatch(state, "guest-leaf", "bzero-unavailable", target);
      trace_guest_clock(state, "guest-memory-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-memory-return", target);
    };
    if (length <= 0) {
      if (state.guest_leaf_probe) guest_bzero();
      return 1;
    }
    state.active_bzero_address = a0;
    state.active_bzero_length = a1;
    std::size_t offset = 0U;
    if (!resolve_bzero_host_offset(state, a0,
                                   static_cast<std::uint32_t>(length),
                                   offset)) {
      const std::uint32_t caller =
          state.recent_dispatch_position < 2U
              ? 0U
              : state.recent_dispatches[
                    (state.recent_dispatch_position - 2U) %
                    state.recent_dispatches.size()];
      const std::uint32_t parent =
          state.recent_dispatch_position < 3U
              ? 0U
              : state.recent_dispatches[
                    (state.recent_dispatch_position - 3U) %
                    state.recent_dispatches.size()];
      std::array<char, 128U> operation{};
      (void)std::snprintf(operation.data(), operation.size(),
                          "bzero-%08x-%08x-ra%08x-sp%08x-c%08x-p%08x", a0,
                          a1,
                          static_cast<std::uint32_t>(context->r31),
                          static_cast<std::uint32_t>(context->r29), caller,
                          parent);
      fail_closed_dispatch(state, "memory", operation.data(), target);
    }
    if (state.guest_leaf_probe) {
      guest_bzero();
    } else {
      for (std::uint32_t index = 0U;
           index < static_cast<std::uint32_t>(length); ++index) {
        state.rdram[(offset + index) ^ 3U] = 0U;
      }
    }
    state.active_bzero_address = 0U;
    state.active_bzero_length = 0U;
    return 1;
  }
  if (std::strcmp(name, "osVirtualToPhysical") == 0) {
    const std::uint32_t segment = a0 & 0xE0000000U;
    const std::uint32_t physical =
        segment == 0x80000000U || segment == 0xA0000000U
            ? a0 & 0x1FFFFFFFU
            : a0;
    set_result(context, static_cast<std::int32_t>(physical));
    return 1;
  }
  if (std::strcmp(name, "osSetIntMask") == 0) {
    const std::uint32_t previous = state.interrupt_mask;
    state.interrupt_mask = a0;
    set_result(context, static_cast<std::int32_t>(previous));
    state.journal.append(NativeEventKind::kInterruptMask, previous, a0);
    return 1;
  }
  if (std::strcmp(name, "osWritebackDCacheAll") == 0) {
    state.journal.append(NativeEventKind::kCacheMaintenance, 1U);
    if (state.guest_leaf_probe) {
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr)
        fail_closed_dispatch(state, "guest-leaf", "cache-unavailable", target);
      trace_guest_clock(state, "guest-cache-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-cache-return", target);
    }
    return 1;
  }
  if (std::strcmp(name, "osWritebackDCache") == 0 ||
      std::strcmp(name, "osInvalDCache") == 0 ||
      std::strcmp(name, "osInvalICache") == 0) {
    const auto length = static_cast<std::int32_t>(a1);
    if (length > 0) {
      const std::uint32_t segment = a0 & 0xE0000000U;
      if ((segment != 0x80000000U && segment != 0xA0000000U) ||
          (a0 & 0x1FFFFFFFU) >= kRdramSize ||
          static_cast<std::uint64_t>(a0 & 0x1FFFFFFFU) +
                  static_cast<std::uint32_t>(length) >
              kRdramSize) {
        fail_closed_dispatch(state, "hle", "invalid-cache-range", target);
      }
    }
    const std::uint32_t operation =
        std::strcmp(name, "osWritebackDCache") == 0 ? 2U
        : std::strcmp(name, "osInvalDCache") == 0   ? 3U
                                                      : 4U;
    state.journal.append(NativeEventKind::kCacheMaintenance, operation, a0,
                         a1);
    if (state.guest_leaf_probe) {
      if (static_cast<std::int64_t>(context->r5) != static_cast<std::int64_t>(length))
        fail_closed_dispatch(state, "guest-leaf", "cache-length-extension", target);
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr)
        fail_closed_dispatch(state, "guest-leaf", "cache-unavailable", target);
      trace_guest_clock(state, "guest-cache-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-cache-return", target);
    }
    return 1;
  }
  if (std::strcmp(name, "osViSetMode") == 0) {
    if (a0 < kKseg0 || a0 - kKseg0 > kRdramSize - 0x50U) {
      fail_closed_dispatch(state, "hle", "invalid-vi-mode", target);
    }
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    if (!configure_live_vi_mode(state, memory, a0))
      fail_closed_dispatch(state, "hle", "invalid-vi-mode", target);
#endif
    state.vi_mode = a0;
    return 1;
  }
  if (std::strcmp(name, "osViBlack") == 0) {
    state.vi_blacked = a0 != 0U;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    state.rt64_vi.horizontal_start =
        state.vi_blacked ? 0U : state.vi_mode_horizontal_start;
#endif
    return 1;
  }
  if (std::strcmp(name, "osViSetEvent") == 0) {
    if (!state.queues.contains(a0) || a2 == 0U) {
      fail_closed_dispatch(state, "hle", "invalid-vi-event", target);
    }
    state.vi_queue = a0;
    state.vi_message = a1;
    state.vi_retrace_interval = a2;
    state.journal.append(NativeEventKind::kViConfigured, a0, a1, a2);
    return 1;
  }
  if (std::strcmp(name, "osViSwapBuffer") == 0) {
    ++state.vi_swap_calls;
    if (a0 < kKseg0 || a0 - kKseg0 >= kRdramSize) {
      fail_closed_dispatch(state, "hle", "invalid-framebuffer", target);
    }
    state.vi_current_framebuffer = state.vi_next_framebuffer;
    state.vi_next_framebuffer = a0;
    return 1;
  }
  if (std::strcmp(name, "osViGetCurrentFramebuffer") == 0) {
    set_result(context,
               static_cast<std::int32_t>(state.vi_current_framebuffer));
    return 1;
  }
  if (std::strcmp(name, "osViGetNextFramebuffer") == 0) {
    set_result(context, static_cast<std::int32_t>(state.vi_next_framebuffer));
    return 1;
  }
  if (std::strcmp(name, "osViSetSpecialFeatures") == 0) {
    state.vi_special_features = a0;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    state.rt64_vi.status = jfg::vi_apply_special_features(
        state.rt64_vi.status, state.vi_mode_control, a0);
    state.gameplay_trace.event("vi-features", {state.vi_retraces, a0, state.rt64_vi.status});
#endif
    return 1;
  }
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  if (!state.guest_leaf_probe && std::strcmp(name, "osSpTaskYield") == 0) {
    // A yield request gives waiting audio a chance to run. Our RSP backend
    // cannot suspend a task, so finish the pending graphics work now through
    // its sole SP/DP completion path. Waiting for another VI here delays the
    // audio task and changes the guest scheduler's buffer/frame cadence.
    // This neither advances the VI clock nor resubmits a graphics task.
    if (++state.rsp_yield_requests == 1U)
      jfg::support_event("rsp=yield-request-run-to-completion");
#if defined(_WIN32)
    state.gameplay_trace.event("rsp-yield", {state.vi_retraces,
        state.controller_samples, state.gameplay_trace_updates,
        state.pending_graphics_tasks.size(), state.rsp_yield_requests});
    complete_pending_live_graphics_tasks(state, target);
#endif
    return 1;
  }
  if (!state.guest_leaf_probe && std::strcmp(name, "osSpTaskYielded") == 0) {
    // The scheduler asks after SP completion. No task was suspended, so it
    // must retire this task rather than enqueueing it again as yielded.
    set_result(context, 0);
    if (++state.rsp_yield_queries == 1U)
      jfg::support_event("rsp=yield-query-completed");
    return 1;
  }
#endif
  hle::HleFunction function{};
  if (std::strcmp(name, "osCreateMesgQueue") == 0) {
    function = hle::HleFunction::kOsCreateMesgQueue;
    ++state.queue_create_calls;
  } else if (std::strcmp(name, "osSendMesg") == 0) {
    function = hle::HleFunction::kOsSendMesg;
    ++state.queue_send_calls;
  } else if (std::strcmp(name, "osJamMesg") == 0) {
    function = hle::HleFunction::kOsJamMesg;
    ++state.queue_send_calls;
  } else if (std::strcmp(name, "osRecvMesg") == 0) {
    function = hle::HleFunction::kOsRecvMesg;
    ++state.queue_recv_calls;
  } else if (std::strcmp(name, "osSpTaskLoad") == 0) {
    ++state.sp_task_load_calls;
    std::uint32_t task_type = 0U;
    if (a0 == 0U || !memory.read_u32(a0, task_type) || task_type > 7U)
      fail_closed_dispatch(state, "hle", "invalid-rsp-task", target);
    if (state.guest_leaf_probe) {
      const auto stack = static_cast<std::uint32_t>(context->r29);
      std::uint32_t flags = 0U;
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr || state.mmio_trace == nullptr ||
          !state.mmio_trace->guest_sp_status ||
          (state.mmio_trace->sp_status.read() & 0x1dU) != 1U ||
          !memory.read_u32(a0 + 4U, flags) || (flags & 1U) != 0U ||
          (stack & 7U) != 0U || stack < kKseg0 + 0x80U ||
          stack > kKseg0 + kRdramSize - 4U)
        fail_closed_dispatch(state, "guest-leaf", "rsp-load-state", target);
      const auto transfers = state.mmio_trace->sp_dma.transfers();
      trace_guest_clock(state, "guest-sp-load-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-sp-load-return", target);
      if (static_cast<std::uint32_t>(context->r29) != stack ||
          (state.mmio_trace->sp_status.read() & 0x7dU) != 0x41U ||
          state.mmio_trace->sp_dma.transfers() - transfers != 2U)
        fail_closed_dispatch(state, "guest-leaf", "rsp-load-result", target);
    }
    state.loaded_rsp_task = a0;
    state.loaded_rsp_task_type = task_type;
    return 1;
  } else if (std::strcmp(name, "osSpTaskStartGo") == 0) {
    ++state.sp_task_start_calls;
    if (a0 == 0U || a0 != state.loaded_rsp_task)
      fail_closed_dispatch(state, "hle", "rsp-task-not-loaded", target);
    if (state.guest_leaf_probe) {
      const auto stack = static_cast<std::uint32_t>(context->r29);
      recomp_func_t *const generated = jfg_generated_lookup_function(address);
      if (generated == nullptr || state.mmio_trace == nullptr ||
          !state.mmio_trace->guest_sp_status ||
          (state.mmio_trace->sp_status.read() & 0x1dU) != 1U ||
          (stack & 7U) != 0U || stack < kKseg0 + 0x20U ||
          stack > kKseg0 + kRdramSize - 4U)
        fail_closed_dispatch(state, "guest-leaf", "rsp-start-state", target);
      trace_guest_clock(state, "guest-sp-start-entry", target);
      generated(rdram, context);
      trace_guest_clock(state, "guest-sp-start-return", target);
      if ((state.mmio_trace->sp_status.read() & 0x7fU) != 0x40U ||
          static_cast<std::uint32_t>(context->r29) != stack)
        fail_closed_dispatch(state, "guest-leaf", "rsp-start-result", target);
    }
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    if (state.loaded_rsp_task_type == 2U) {
#if defined(_WIN32)
      write_private_progress(state, "audio-begin");
#endif
      const auto audio_start = std::chrono::steady_clock::now();
      const bool audio_ok = execute_live_audio_task(state, memory);
      state.audio_task_microseconds += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - audio_start)
              .count());
#if defined(_WIN32)
      write_private_progress(state, "audio-end");
#endif
      if (!audio_ok)
        fail_closed_dispatch(state, "rsp", "audio-task-rejected", target);
#if defined(_WIN32)
      complete_guest_sp_status(state, target);
      write_private_progress(state, "audio-completion-begin");
#endif
      const auto sp = state.events.find(4U);
      if (state.scheduler == nullptr || sp == state.events.end() ||
          state.scheduler->send(sp->second.first, sp->second.second, false) !=
              hle::kOsSuccess)
        fail_closed_dispatch(state, "rsp", "sp-completion", target);
#if defined(_WIN32)
      write_private_progress(state, "audio-completion-end");
#endif
      state.loaded_rsp_task = 0U;
      state.loaded_rsp_task_type = 0U;
      if (state.scheduler_running) {
#if defined(_WIN32)
        write_private_progress(state, "audio-yield-begin");
#endif
        state.scheduler->yield_current();
#if defined(_WIN32)
        write_private_progress(state, "audio-yield-end");
#endif
      }
      return 1;
    }
#if defined(_WIN32)
    if (state.loaded_rsp_task_type == 1U) {
      write_private_progress(state, "graphics-prepare-begin");
      const auto graphics_start = std::chrono::steady_clock::now();
      constexpr std::uint64_t kFastReplayRenderWindow = 240U;
      // A requested RDRAM capture is a renderer diagnostic and must preserve
      // the complete RT64 task history; otherwise a late failure can disappear
      // because all of its prerequisite workloads were intentionally skipped.
      const bool skip_graphics =
          state.fast_replay && !state.renderer_writeback_probe && state.poll_target == 0U &&
          state.rdram_capture_path.empty() &&
          state.vi_retraces + kFastReplayRenderWindow < state.retrace_target;
      bool graphics_ok = true;
      std::uint32_t command_size = 0U;
      if (skip_graphics &&
          !memory.read_u32(state.loaded_rsp_task + 0x34U, command_size))
        graphics_ok = false;
      const bool preserve_immediate_task =
          skip_graphics && is_immediate_offscreen_task_candidate(command_size);
      if (graphics_ok && skip_graphics && !preserve_immediate_task) {
        PendingLiveGraphicsTask pending{};
        pending.skipped = true;
        state.pending_graphics_tasks.push_back(std::move(pending));
      } else if (graphics_ok) {
        graphics_ok = execute_live_graphics_task(state, memory);
      }
      const auto prepare_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - graphics_start)
              .count());
      state.gameplay_trace.event("prepare", {state.vi_retraces, state.controller_samples,
          state.gameplay_trace_updates, prepare_us, state.last_graphics_command_address,
          state.last_graphics_descriptor[5]});
      state.graphics_prepare_microseconds += prepare_us;
      state.graphics_prepare_max_us =
          (std::max)(state.graphics_prepare_max_us, prepare_us);
      if (state.timing_trace) {
        state.timing_trace << "prepare\t" << state.vi_frames << '\t'
                           << state.vi_retraces << '\t' << prepare_us << '\t'
                           << std::hex << state.last_graphics_command_address
                           << std::dec << '\n';
      }
      write_private_progress(state, "graphics-prepare-end");
      if (!graphics_ok) {
        std::array<char, 192U> operation{};
        (void)std::snprintf(
            operation.data(), operation.size(),
            "gfx-%u-c%08x-n%x-m%u-a%08x-w%08x-%08x-"
            "p%08x-%08x-%08x-b%08x-s%08x",
            static_cast<unsigned>(state.last_rt64_error),
            state.last_graphics_command_address,
            state.last_graphics_descriptor[5],
            state.materialize_reject_reason,
            state.materialize_reject_address,
            state.materialize_reject_word0,
            state.materialize_reject_word1,
            state.materialize_parent_address,
            state.materialize_parent_word0,
            state.materialize_parent_word1,
            state.materialize_block_address,
            state.materialize_parent_segment_base);
        fail_closed_dispatch(state, "rsp", operation.data(), target);
      }
      // Tiny actor passes can finish within the same VI as in the oracle.
      // The completion path fails closed if one ever targets a displayed
      // framebuffer; main scene tasks retain VI-boundary completion pacing.
      if (!state.pending_graphics_tasks.empty() &&
          !state.pending_graphics_tasks.back().skipped &&
          is_immediate_offscreen_task_candidate(
              state.last_graphics_descriptor[5]))
        complete_pending_live_graphics_tasks(state, target, true);
      state.loaded_rsp_task = 0U;
      state.loaded_rsp_task_type = 0U;
      if (state.scheduler_running)
        state.scheduler->yield_current();
      return 1;
    }
#endif
#endif
    fail_closed_dispatch(state, "hle",
                         state.loaded_rsp_task_type == 1U
                             ? "osSpTaskStartGo-gfx"
                             : "osSpTaskStartGo-other",
                         target);
  } else {
    fail_closed_dispatch(state, "hle", name, target);
  }
  const bool recv = function == hle::HleFunction::kOsRecvMesg;
  const bool clock_queue = a0 == 0x800FE4B8U || a0 == 0x800FE8A8U ||
                           a0 == 0x800FEB80U;
  if (clock_queue) trace_guest_clock(state, recv ? "recv" : "queue-call", a0);
#if defined(JFG_LIVE_RUNTIME)
  if (state.focus_update_start != 0U &&
      state.completed_game_updates + 1U >= state.focus_update_start - 1U &&
      state.completed_game_updates + 1U <= state.focus_update_end + 1U &&
      state.timing_trace.is_open() &&
      (a0 == 0x800FE4B8U || a0 == 0x800FE8A8U ||
       a0 == 0x800FEB80U)) {
    state.timing_trace << "game-queue-call\t" << state.vi_frames << '\t'
                       << state.vi_retraces << "\t0\t"
                       << (recv ? "recv" :
                           function == hle::HleFunction::kOsCreateMesgQueue
                               ? "create" : "send")
                       << '\t' << state.completed_game_updates << '\t'
                       << state.scheduler->current_thread_id() << '\t'
                       << std::hex << a0 << ':' << a1 << ':' << a2 << ':'
                       << static_cast<std::uint32_t>(context->r31)
                       << std::dec << '\n';
  }
#endif
  if (recv) {
    trace_si_timing(state, "recv-entry", a0,
                    static_cast<std::uint32_t>(context->r31),
                    static_cast<std::int32_t>(a2));
    const auto si = state.events.find(5U);
    if (si != state.events.end() && a0 == si->second.first) {
      ++state.si_receive_calls;
      state.last_si_receive_return = static_cast<std::uint32_t>(context->r31);
      state.last_si_receive_block = a2;
      if (a2 == static_cast<std::uint32_t>(hle::kOsMesgBlock)) {
        for (std::size_t index = 0U;
             index < state.last_si_dispatch_trace.size(); ++index) {
          const std::size_t distance =
              state.last_si_dispatch_trace.size() - index;
          state.last_si_dispatch_trace[index] =
              state.recent_dispatch_position >= distance
                  ? state.recent_dispatches[
                        (state.recent_dispatch_position - distance) %
                        state.recent_dispatches.size()]
                  : 0U;
        }
      }
    }
  }
  const bool send = function == hle::HleFunction::kOsSendMesg ||
                    function == hle::HleFunction::kOsJamMesg;
  if (send) {
    const auto block = static_cast<std::int32_t>(a2);
    if (state.scheduler == nullptr || !state.queues.contains(a0) ||
        (block != hle::kOsMesgNoBlock && block != hle::kOsMesgBlock)) {
      fail_closed_dispatch(state, "hle", "invalid-send", target);
    }
    const auto result = state.scheduler->send(
        a0, a1, function == hle::HleFunction::kOsJamMesg);
    if (result == hle::kOsSuccess) {
      set_result(context, result);
      if (state.scheduler_running)
        state.scheduler->yield_current();
      return 1;
    }
    if (block == hle::kOsMesgNoBlock) {
      set_result(context, hle::kOsFull);
      return 1;
    }
    if (!state.scheduler_running || state.scheduler->current_thread_id() < 0)
      fail_closed_dispatch(state, "hle", "blocking-send-outside-thread", target);
    // Validate the full queue before parking; corrupt state must not become
    // an endless wait. No other guest runs between the failed send and here.
    if (hle::dispatch_queue_call(memory, {function, a0, a1, a2}).disposition !=
        hle::QueueDispatchDisposition::block)
      fail_closed_dispatch(state, "hle", "blocking-send-invalid-queue", target);
    set_result(context, state.scheduler->send_blocking(
        a0, a1, function == hle::HleFunction::kOsJamMesg));
    return 1;
  }
  const auto received_vi_message = [&]() {
    std::uint32_t message = 0U;
    return recv && a0 == state.vi_queue && a1 != 0U &&
           memory.read_u32(a1, message) && message == state.vi_message;
  };
  const auto trace_vi_queue_receive = [&]() {
#if defined(JFG_LIVE_RUNTIME)
    const bool focused_receive = state.focus_update_start != 0U &&
        state.completed_game_updates + 1U >= state.focus_update_start - 1U &&
        state.completed_game_updates + 1U <= state.focus_update_end + 1U;
    if (recv && a0 == state.vi_queue &&
        (state.actor_timing_trace || focused_receive) &&
        state.timing_trace.is_open()) {
      std::uint32_t message = 0U;
      const bool readable = a1 != 0U && memory.read_u32(a1, message);
      state.timing_trace << "vi-queue-receive\t" << state.vi_frames << '\t'
                         << state.vi_retraces << "\t0\t"
                         << (readable ? std::to_string(message) : "unreadable")
                         << '\t' << state.vi_message << '\t'
                         << state.vi_messages_delivered << '\t'
                         << (received_vi_message() &&
                             state.vi_messages_delivered > state.vi_retraces)
                         << '\n';
    }
#endif
  };
  const auto result = hle::dispatch_queue_call(memory, {function, a0, a1, a2});
  if (state.guest_leaf_probe && recv && a1 == 0U && a2 == 0U &&
      result.disposition == hle::QueueDispatchDisposition::accepted &&
      result.return_value == hle::kOsEmpty) {
    // The validated empty nonblocking path made no queue mutation and never
    // follows guest wait lists. Execute its original Status/save/restore path.
    // Other queue paths still require a guest-thread bridge; they are NOT
    // silently declared instruction-accounted by this partial probe.
    const auto sp = static_cast<std::uint32_t>(context->r29);
    if ((sp & 7U) != 0U || sp < kKseg0 + 0x28U ||
        sp > kKseg0 + kRdramSize - 12U)
      fail_closed_dispatch(state, "guest-leaf", "queue-stack-range", target);
    recomp_func_t *const generated = jfg_generated_lookup_function(address);
    if (generated == nullptr)
      fail_closed_dispatch(state, "guest-leaf", "queue-unavailable", target);
    const auto status = cop0_status_read(context);
    trace_guest_clock(state, "guest-queue-empty-entry", target);
    generated(rdram, context);
    trace_guest_clock(state, "guest-queue-empty-return", target);
    if (static_cast<std::int64_t>(context->r2) != hle::kOsEmpty ||
        static_cast<std::uint32_t>(context->r29) != sp || cop0_status_read(context) != status)
      fail_closed_dispatch(state, "guest-leaf", "queue-empty-result", target);
  }
  if (recv && result.disposition == hle::QueueDispatchDisposition::accepted)
    trace_si_timing(state, "recv-return", a0,
                    static_cast<std::uint32_t>(context->r31), result.return_value);
  if (result.disposition == hle::QueueDispatchDisposition::accepted) {
    if (recv && clock_queue) trace_guest_clock(state, "recv-return", a0);
    if (function == hle::HleFunction::kOsCreateMesgQueue) {
      state.queues.insert(a0);
      state.journal.append(NativeEventKind::kQueueCreated, a0, a1, a2);
    }
    if (function != hle::HleFunction::kOsCreateMesgQueue) {
      set_result(context, result.return_value);
    }
    if (recv && result.return_value == hle::kOsSuccess && state.scheduler != nullptr)
      state.scheduler->notify_received(a0);
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    if (recv) {
      state.last_receive_queue = a0;
      state.last_receive_block = a2;
      state.last_receive_result = result.return_value;
      if (result.return_value == hle::kOsSuccess)
        ++state.receive_successes;
      else
        ++state.receive_empties;
    }
    if (recv && result.return_value == hle::kOsSuccess) {
      state.empty_receive_polls.erase(a0);
    } else if (recv &&
               a2 == static_cast<std::uint32_t>(hle::kOsMesgNoBlock)) {
      std::uint32_t &poll_count = state.empty_receive_polls[a0];
      ++poll_count;
      if (poll_count >= kEmptyReceivePollTimeslice &&
          state.scheduler_running && state.scheduler != nullptr &&
          state.scheduler->current_thread_id() >= 0 &&
          state.vi_internal_queue != 0U) {
        state.empty_receive_polls.clear();
        ++state.interrupt_timeslices;
        state.scheduler->preempt_current();
      }
    }
#endif
    if (recv && result.return_value == hle::kOsSuccess)
      trace_vi_queue_receive();
    if (function == hle::HleFunction::kOsRecvMesg &&
        result.return_value == hle::kOsSuccess && received_vi_message() &&
        state.vi_messages_delivered > state.vi_retraces) {
      ++state.vi_retraces;
      jfg::support_breadcrumb(state.vi_frames, state.vi_retraces, state.controller_samples);
#if defined(JFG_LIVE_RUNTIME)
      trace_phase9_event(state, "vi-consumed", state.controller_samples);
#endif
      state.journal.append(NativeEventKind::kViMessageConsumed, a0, a1,
                           static_cast<std::uint32_t>(state.vi_retraces));
#if defined(JFG_LIVE_RUNTIME)
      write_retrace_semantic_hash(state);
      write_private_progress(state);
#endif
      if (state.vi_retraces >= state.retrace_target)
        retrace_gate(state);
    }
    return 1;
  }
  if (result.disposition == hle::QueueDispatchDisposition::reject) {
    const auto block = static_cast<std::int32_t>(a2);
    if (!state.queues.contains(a0)) {
      std::array<char, 32U> operation{};
      (void)std::snprintf(operation.data(), operation.size(),
                          "unknown-queue-%08x", a0);
      fail_closed_dispatch(state, "hle", operation.data(), target);
    }
    const char *reason = block != hle::kOsMesgNoBlock &&
                                   block != hle::kOsMesgBlock
                             ? "invalid-block-mode"
                         : recv && a1 != 0U &&
                                   (a1 < kKseg0 || a1 - kKseg0 >
                                                       kRdramSize - 4U)
                             ? "invalid-message-output"
                             : "corrupt-queue";
    fail_closed_dispatch(state, "hle", reason, target);
  }
  if (recv && state.scheduler != nullptr && state.scheduler_running) {
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    ++state.receive_blocks;
    state.last_receive_queue = a0;
    state.last_receive_block = a2;
#endif
    const std::int32_t receive_result =
        state.scheduler->recv_blocking(a0, a1);
    if (clock_queue) trace_guest_clock(state, "recv-resume", a0);
    trace_si_timing(state, "recv-resume", a0,
                    static_cast<std::uint32_t>(context->r31), receive_result);
    set_result(context, receive_result);
    if (receive_result == hle::kOsSuccess)
      trace_vi_queue_receive();
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    state.last_receive_result = receive_result;
    if (receive_result == hle::kOsSuccess)
      ++state.receive_successes;
#endif
    if (receive_result == hle::kOsSuccess && received_vi_message() &&
        state.vi_messages_delivered > state.vi_retraces) {
      ++state.vi_retraces;
      jfg::support_breadcrumb(state.vi_frames, state.vi_retraces, state.controller_samples);
#if defined(JFG_LIVE_RUNTIME)
      trace_phase9_event(state, "vi-consumed", state.controller_samples);
#endif
      state.journal.append(NativeEventKind::kViMessageConsumed, a0, a1,
                           static_cast<std::uint32_t>(state.vi_retraces));
#if defined(JFG_LIVE_RUNTIME)
      write_retrace_semantic_hash(state);
      write_private_progress(state);
#endif
      if (state.vi_retraces >= state.retrace_target)
        retrace_gate(state);
    }
    return 1;
  }
  fail_closed_dispatch(state, "hle",
                       recv ? "blocking-recv"
                            : (function == hle::HleFunction::kOsJamMesg
                                   ? "blocking-jam"
                                   : "blocking-send"),
                       target);
}

int native_cpu_operation(void* opaque, void* guest_context, std::uint32_t operation,
                         std::uint32_t selector, std::uint64_t* value) {
  auto& state = *static_cast<State*>(opaque);
  if (!state.guest_leaf_probe || guest_context == nullptr || value == nullptr) return 0;
  // RGO fast clock: COP0 reads Count and writes can change timers or
  // interrupts, so account and end any skip run first.
  jfg_fast_run_end(&state);
  auto* context = static_cast<recomp_context*>(guest_context);
  // This bridge has only a 32-bit virtual-address model. Do not pretend
  // that enabling extended addressing supplies a compatible EntryHi state.
  if ((static_cast<std::uint32_t>(cop0_status_read(context)) & 0xe0U) != 0U) return 0;
  if (state.guest_os_probe) {
    const auto extend = [](std::uint32_t word) {
      return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)));
    };
    if (operation == JFG_CPU_READ_REGISTER) {
      if (selector == 9U) { *value = extend(static_cast<std::uint32_t>(state.cpu_count)); return 1; }
      if (selector == 11U) { *value = extend(state.guest_timer.read()); return 1; }
      if (selector == 13U) {
        *value = extend(state.guest_cause |
            (state.guest_timer.interrupt() ? 0x8000U : 0U) |
            ((state.mmio_trace->mi_pending & state.mmio_trace->mi_mask.read()) ? 0x400U : 0U));
        return 1;
      }
      if (selector == 14U) { *value = extend(state.guest_epc); return 1; }
    }
    if (operation == JFG_CPU_WRITE_REGISTER) {
      state.execution_fast_deadline = 0U;
      if (selector == 11U) {
        if (!state.guest_timer.write(state.cpu_count, static_cast<std::uint32_t>(*value)))
          fail_closed_dispatch(state, "guest-cpu", "unqualified-compare-write", state.guest_last_pc);
        if (state.guest_clock_trace)
          state.guest_clock_trace << "compare\t" << state.cpu_count << '\t'
              << static_cast<std::uint32_t>(*value) << '\n';
        return 1;
      }
      if (selector == 13U) {
        state.guest_cause = (state.guest_cause & ~0x300U) | (static_cast<std::uint32_t>(*value) & 0x300U);
        return 1;
      }
      if (selector == 14U) { state.guest_epc = static_cast<std::uint32_t>(*value); return 1; }
    }
    if (operation == JFG_CPU_EXCEPTION_RETURN) {
      std::uint32_t next = 0U;
      hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
      if ((context->status_reg & 6U) != 2U || state.guest_transport == nullptr ||
          !memory.read_u32(0x800a9e90U, next) || next < kKseg0 || next > kKseg0 + kRdramSize - 0x1b0U)
        return 0;
      cop0_status_write(context, context->status_reg & ~2U);
      state.guest_previous_branch = state.guest_previous_delay = false;
      state.guest_eret_boundary = true;
      state.guest_transport->eret(next, state.guest_epc, *context);
      return 1;
    }
  }
  if (operation == JFG_CPU_CACHE_OPERATION)
    return reference_coherent_cache_operation(selector, *value, kRdramSize) ? 1 : 0;
  if (operation == JFG_CPU_READ_REGISTER) {
    // Only the independently qualified cold-boot osInitialize path. The
    // cooperative runtime does not model live Cause/exception delivery.
    if (selector == 13U && !state.os_initialized && !state.scheduler_running) {
      *value = 0U;
      return 1;
    }
    const auto result = state.tlb_registers.read(selector);
    if (!result) return 0;
    *value = static_cast<std::uint64_t>(static_cast<std::int64_t>(
        static_cast<std::int32_t>(*result)));
    return 1;
  }
  if (operation == JFG_CPU_WRITE_REGISTER)
    return state.tlb_registers.write(selector, static_cast<std::uint32_t>(*value)) ? 1 : 0;
  if (operation == JFG_CPU_TLB_OPERATION) {
    switch (selector) {
    case 0: return state.tlb_registers.probe() ? 1 : 0;
    case 1: return state.tlb_registers.read_indexed() ? 1 : 0;
    case 2: return state.tlb_registers.write_indexed() ? 1 : 0;
    default: return 0;
    }
  }
  return 0;
}

struct DispatchBinding {
  State &state;
  bool bound = false;
  bool cpu_bound = false;
  ~DispatchBinding() {
    if (cpu_bound)
      (void)jfg_minimal_runtime_unbind_cpu(native_cpu_operation, &state);
    if (bound)
      (void)jfg_minimal_runtime_unbind_dispatch(dispatch, &state);
  }
};
bool range(std::uint64_t offset, std::uint64_t bytes, std::size_t limit) {
  return offset <= limit && bytes <= limit - offset;
}
bool write_boot_word(std::vector<std::uint8_t> &bytes, std::uint32_t address,
                     std::uint32_t value) {
  if (address < kKseg0 || !range(address - kKseg0, 4U, bytes.size()))
    return false;
  const std::size_t offset = address - kKseg0;
  bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
  bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 16U);
  bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 8U);
  bytes[offset + 3U] = static_cast<std::uint8_t>(value);
  return true;
}
bool initialize_ipl_state(std::vector<std::uint8_t> &bytes,
                          std::span<const std::uint8_t> rom) {
  // Documented libultra boot globals populated by the IPL before the ROM
  // entry point. This runner admits the USA image, so the TV mode is NTSC;
  // it models a cold cartridge boot with the oracle-observed 4 MiB RDRAM.
  return initialize_6105_handoff(bytes, rom) &&
         write_boot_word(bytes, 0x80000300U, 1U) &&
         write_boot_word(bytes, 0x80000304U, 0U) &&
         write_boot_word(bytes, 0x80000308U, 0xB0000000U) &&
         write_boot_word(bytes, 0x8000030CU, 0U) &&
         write_boot_word(bytes, 0x80000318U,
                         static_cast<std::uint32_t>(kRdramSize));
}
bool load_static_sections(const std::vector<std::uint8_t> &rom,
                          std::vector<std::uint8_t> &bytes) {
  const std::size_t count = jfg_generated_section_count();
  for (std::size_t index = 0U; index < count; ++index) {
    JfgGeneratedSectionMetadata metadata{};
    if (jfg_generated_section_metadata(static_cast<std::uint32_t>(index),
                                       &metadata) == 0 ||
        metadata.is_overlay > 1U)
      return false;
    if (metadata.is_overlay == 1U)
      continue;
    if (metadata.linked_vram < kKseg0 || (metadata.linked_vram & 3U) != 0U ||
        (metadata.rom_start & 3U) != 0U ||
        metadata.text_rom_offset > metadata.text_size)
      return false;
    const std::uint64_t copy =
        std::uint64_t(metadata.text_size) + metadata.data_size;
    const std::uint64_t total = copy + metadata.bss_size;
    const std::uint64_t destination = metadata.linked_vram - kKseg0;
    if (!range(metadata.rom_start, copy, rom.size()) ||
        !range(destination, total, bytes.size()))
      return false;
    std::memcpy(bytes.data() + destination, rom.data() + metadata.rom_start,
                static_cast<std::size_t>(copy));
    std::memset(bytes.data() + destination + copy, 0, metadata.bss_size);
  }
  return true;
}

void invoke_generated(const jfg::GeneratedOverlayFunctionHandle &function,
                      std::uint8_t *rdram, recomp_context *context,
                      State &state, std::uint32_t transfer_target) {
  if (function.invoke(rdram, context) != jfg::GeneratedOverlayError::none) {
    ledger(state, "generated", "invoke-failed", transfer_target);
  }
}

std::array<std::uint8_t, 20U> sha1(std::span<const std::uint8_t> input) {
  std::array<std::uint32_t, 5U> hash = {0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                        0x10325476U, 0xc3d2e1f0U};
  std::vector<std::uint8_t> padded(input.begin(), input.end());
  padded.push_back(0x80U);
  while ((padded.size() % 64U) != 56U)
    padded.push_back(0U);
  const std::uint64_t bits = static_cast<std::uint64_t>(input.size()) * 8U;
  for (int shift = 56; shift >= 0; shift -= 8)
    padded.push_back(static_cast<std::uint8_t>(bits >> shift));
  for (std::size_t block = 0U; block < padded.size(); block += 64U) {
    std::array<std::uint32_t, 80U> words{};
    for (std::size_t index = 0U; index < 16U; ++index)
      words[index] = (std::uint32_t(padded[block + index * 4U]) << 24U) |
                     (std::uint32_t(padded[block + index * 4U + 1U]) << 16U) |
                     (std::uint32_t(padded[block + index * 4U + 2U]) << 8U) |
                     padded[block + index * 4U + 3U];
    for (std::size_t index = 16U; index < words.size(); ++index)
      words[index] = std::rotl(words[index - 3U] ^ words[index - 8U] ^
                                   words[index - 14U] ^ words[index - 16U],
                               1);
    auto [a, b, c, d, e] = hash;
    for (std::size_t index = 0U; index < words.size(); ++index) {
      const std::uint32_t f = index < 20U   ? ((b & c) | (~b & d))
                              : index < 40U ? (b ^ c ^ d)
                              : index < 60U ? ((b & c) | (b & d) | (c & d))
                                            : (b ^ c ^ d);
      const std::uint32_t k = index < 20U   ? 0x5a827999U
                              : index < 40U ? 0x6ed9eba1U
                              : index < 60U ? 0x8f1bbcdcU
                                            : 0xca62c1d6U;
      const std::uint32_t next = std::rotl(a, 5) + f + e + k + words[index];
      e = d;
      d = c;
      c = std::rotl(b, 30);
      b = a;
      a = next;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
  }
  std::array<std::uint8_t, 20U> result{};
  for (std::size_t index = 0U; index < hash.size(); ++index)
    for (std::size_t byte = 0U; byte < 4U; ++byte)
      result[index * 4U + byte] =
          static_cast<std::uint8_t>(hash[index] >> (24U - byte * 8U));
  return result;
}
} // namespace

// Called only by an explicitly instrumented, private generated-code copy.
// An ordinary generated root contains no call sites and is unaffected.
extern "C" void jfg_phase9_instruction_probe(uint32_t pc, uint8_t *rdram,
                                               recomp_context *context) {
#if defined(JFG_LIVE_RUNTIME)
  State *const state = g_active_child_state;
  if (state == nullptr || !state->instruction_probe_trace.is_open() ||
      rdram != state->rdram || context == nullptr ||
      state->focus_update_start == 0U ||
      state->completed_game_updates + 1U < state->focus_update_start - 1U ||
      state->completed_game_updates + 1U > state->focus_update_end + 1U)
    return;
  if (++state->instruction_probe_hits > 1024U) {
    std::fputs("native instruction probe overflow\n", stderr);
    std::abort();
  }
  auto &out = state->instruction_probe_trace;
  out << state->completed_game_updates + 1U << '\t'
      << state->vi_retraces << '\t' << state->controller_samples << '\t'
      << "0x" << std::hex << std::setw(8) << std::setfill('0') << pc;
  const std::array<std::uint64_t, 11U> gprs = {
      context->r4, context->r5, context->r6, context->r7,
      context->r10, context->r15, context->r19, context->r20,
      context->r21, context->r23, context->r25};
  for (const auto value : gprs)
    out << '\t' << "0x" << std::setw(8)
        << static_cast<std::uint32_t>(value);
  out << '\t' << "0x" << std::setw(8) << context->f4.u32l
      << '\t' << "0x" << std::setw(8) << context->f22.u32l
      << '\t' << "0x" << std::setw(8)
      << (context->f_odd != nullptr ? context->f_odd[(23U - 1U) * 2U] : 0U)
      << std::dec << '\t' << std::fegetround() << '\n';
  out.flush();
#else
  (void)pc;
  (void)rdram;
  (void)context;
#endif
}

#if defined(JFG_LIVE_RUNTIME)
// Host-only diagnostic data. Instruction-entry/successor samples do not by
// themselves prove retirement, a completed receive or caller equivalence.
static void emit_device_event(State& state, const recomp_context& cpu,
    unsigned phase, unsigned source, std::uint32_t pc, std::uint32_t word,
    std::optional<std::uint64_t> deadline = std::nullopt) {
  if (!state.device_event_stream || state.device_event_probe.complete) return;
  const auto invocation = state.completed_game_updates + 1U;
  if (invocation < state.device_event_probe.first) return;
  if (invocation > state.device_event_probe.last) {
    if (!jfg_device_event_finish(&state.device_event_probe))
      fail_closed_dispatch(state, "device-event-probe", "output-failed", pc);
    return;
  }
  jfg_device_event_row row{};
  row.invocation = static_cast<std::uint32_t>(invocation);
  row.phase = phase; row.source = source; row.pc = pc; row.opcode = word;
  row.previous_pc = state.device_event_previous_pc;
  row.previous_opcode = state.device_event_previous_word;
  row.clock_raw = static_cast<std::uint32_t>(state.cpu_count);
  row.clock_anchor_pc = pc;
  row.deadline_valid = deadline.has_value() ? 1U : 0U;
  row.deadline = static_cast<std::uint32_t>(deadline.value_or(0));
  row.status = cpu.status_reg;
  row.mi_pending = state.mmio_trace->mi_pending;
  row.mi_mask = state.mmio_trace->mi_mask.read();
  // Same pure value composition as JFG_CPU_READ_REGISTER selector 13; do not
  // report only the software latch while omitting pending hardware lines.
  row.cause = state.guest_cause | (state.guest_timer.interrupt() ? 0x8000U : 0U) |
      ((row.mi_pending & row.mi_mask) ? 0x400U : 0U);
  hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
  if (!memory.read_u32(0x800a9e90U, row.thread))
    fail_closed_dispatch(state, "device-event-probe", "thread-read", pc);
  const std::array<std::uint64_t, 32> registers = {
    0, cpu.r1, cpu.r2, cpu.r3, cpu.r4, cpu.r5, cpu.r6, cpu.r7,
    cpu.r8, cpu.r9, cpu.r10, cpu.r11, cpu.r12, cpu.r13, cpu.r14, cpu.r15,
    cpu.r16, cpu.r17, cpu.r18, cpu.r19, cpu.r20, cpu.r21, cpu.r22, cpu.r23,
    cpu.r24, cpu.r25, cpu.r26, cpu.r27, cpu.r28, cpu.r29, cpu.r30, cpu.r31};
  std::copy(registers.begin(), registers.end(), row.gpr);
  if (!jfg_device_event_emit(&state.device_event_probe, &row))
    fail_closed_dispatch(state, "device-event-probe", "output-or-budget", pc);
}

static void emit_instruction_effect(State& state, const recomp_context& cpu,
    unsigned phase, unsigned section, unsigned pc, unsigned word,
    std::uint32_t target = 0U, std::uint32_t selected_owner = 0U) {
  if (!state.instruction_effect_stream.is_open()) return;
  const auto invocation = static_cast<std::uint32_t>(state.completed_game_updates + 1U);
  if (!state.instruction_effect_trace.advance(invocation))
    fail_closed_dispatch(state, "instruction-effect-probe", "incomplete-or-output", pc);
  if (invocation != state.instruction_effect_update) return;
  if (!state.guest_os_probe || !state.guest_transport || !state.device_event_stream ||
      state.device_event_probe.complete)
    fail_closed_dispatch(state, "instruction-effect-probe", "unqualified-runtime", pc);
  InstructionEffectRow row{};
  row.phase = phase; row.section = section; row.pc = pc; row.opcode = word;
  row.owner = state.guest_transport->current();
  row.count = static_cast<std::uint32_t>(state.cpu_count);
  row.status = cpu.status_reg;
  row.cause = state.guest_cause | (state.guest_timer.interrupt() ? 0x8000U : 0U) |
      ((state.mmio_trace->mi_pending & state.mmio_trace->mi_mask.read()) ? 0x400U : 0U);
  row.target = target; row.selected_owner = selected_owner;
  row.device_sequence = state.device_event_probe.rows;
  hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
  if (!memory.read_u32(0x800a9e90U, row.thread_word))
    fail_closed_dispatch(state, "instruction-effect-probe", "thread-read", pc);
  row.gpr = {0, cpu.r1, cpu.r2, cpu.r3, cpu.r4, cpu.r5, cpu.r6, cpu.r7,
    cpu.r8, cpu.r9, cpu.r10, cpu.r11, cpu.r12, cpu.r13, cpu.r14, cpu.r15,
    cpu.r16, cpu.r17, cpu.r18, cpu.r19, cpu.r20, cpu.r21, cpu.r22, cpu.r23,
    cpu.r24, cpu.r25, cpu.r26, cpu.r27, cpu.r28, cpu.r29, cpu.r30, cpu.r31};
  if (!state.instruction_effect_trace.emit(invocation, row))
    fail_closed_dispatch(state, "instruction-effect-probe", "pair-boundary-or-budget", pc);
  if (phase == 0U) state.instruction_effect_section = section;
}
#endif

// The generated entry hook is after execution_probe returns: asynchronous
// exception processing must finish before this instruction becomes pending.
extern "C" void jfg_phase9_instruction_effect_entry(unsigned section, unsigned pc,
    unsigned word, unsigned qualification, void* context) {
#if defined(JFG_LIVE_RUNTIME)
  if (g_active_child_state != nullptr && context != nullptr) {
    auto& state = *g_active_child_state;
    if (!state.instruction_effect_stream.is_open()) return;
    if (state.completed_game_updates + 1U == state.instruction_effect_update &&
        (qualification > 1U || (qualification == 1U) != (word == 0x42000018U)))
      fail_closed_dispatch(state, "instruction-effect-probe", "unqualified-instruction", pc);
    emit_instruction_effect(state, *static_cast<recomp_context*>(context), 0U, section, pc, word);
  }
#else
  (void)section; (void)pc; (void)word; (void)qualification; (void)context;
#endif
}

extern "C" void jfg_phase9_instruction_effect(unsigned section, unsigned pc,
    unsigned word, void* context) {
#if defined(JFG_LIVE_RUNTIME)
  if (g_active_child_state != nullptr && context != nullptr)
    emit_instruction_effect(*g_active_child_state, *static_cast<recomp_context*>(context),
                            1U, section, pc, word);
#else
  (void)section; (void)pc; (void)word; (void)context;
#endif
}

// Only called by a separately regenerated private observation root. Count
// entered instruction sites, including loop iterations and executed slots.
// The default is observation-only. The separate guest-OS diagnostic executes
// original OS bodies, with the bounded reference CPU/device profile below.
extern "C" __declspec(noinline) void jfg_phase9_execution_probe_slow(
    unsigned section, unsigned pc, unsigned word, void* context);

// Keep the common instruction path independent of the rare device/exception
// path's large stack frame. The same predicate, cost and state updates run.
extern "C" void jfg_phase9_execution_probe(unsigned section, unsigned pc,
                                          unsigned word, void* context) {
#if defined(JFG_LIVE_RUNTIME)
  if (g_active_child_state != nullptr && context != nullptr) {
    auto& state = *g_active_child_state;
    jfg_fast_run_end(&state);
    // Private optimization: ordinary instructions before the next event need
    // only the same cost and pipeline bookkeeping. Hardware writes, interrupt
    // eligibility, exception return and observers fall back to the full path.
    if (state.execution_fast_profile_ready &&
        !state.guest_eret_boundary && word != 0x1000FFFFU &&
        pc != 0x8009AB80U && pc != 0x80098B74U &&
        (state.play_mode || state.observed_guest_instructions < 24000000000ULL) &&
        state.execution_fast_deadline > state.cpu_count &&
        state.execution_fast_revision == state.mmio_trace->execution_io_revision) {
      auto& cpu = *static_cast<recomp_context*>(context);
      const unsigned primary = word >> 26U;
      const bool fpu = primary == 0x11U || primary == 0x31U ||
          primary == 0x35U || primary == 0x39U || primary == 0x3DU;
      const bool eligible = (cpu.status_reg & 7U) == 1U &&
          (state.execution_fast_pending & cpu.status_reg & 0xFF00U) != 0U;
      const bool park = primary == 3U &&
          (((pc + 4U) & 0xF0000000U) | ((word & 0x03FFFFFFU) << 2U)) == 0x80075698U;
      if (!eligible && !(fpu && (cpu.status_reg & 0x20000000U) == 0U) && !park) {
        ++state.observed_guest_instructions;
        ++state.execution_fast_hits;
        const bool delay = state.guest_previous_branch && pc == state.guest_last_pc + 4U;
        jfg_clock_charge(&state, state.fast_clock != 0U ? 0U :
            jfg_full_cache_cost(pc,word,&cpu,state.rdram));
        state.mmio_trace->guest_count = state.cpu_count;
        state.guest_last_pc = pc;
        state.guest_last_word = word;
        state.guest_previous_delay = delay;
        state.guest_previous_branch = primary == 1U || (primary >= 2U && primary <= 7U) ||
            (primary >= 20U && primary <= 23U) ||
            (primary == 0U && ((word & 63U) == 8U || (word & 63U) == 9U)) ||
            (primary == 0x11U && ((word >> 21U) & 31U) == 8U);
        return;
      }
    }
  }
#endif
  jfg_phase9_execution_probe_slow(section,pc,word,context);
}

// RGO fast clock: let the generated hooks skip the following ordinary
// instructions (jfg_fast_run_sync in original_timing_fast.h). Only when no
// interrupt is pending, even a masked one, so a status write inside the run
// cannot make one deliverable; FPU instructions only while the FPU is
// usable (jfg_fast_fpu); and only up to a few ticks before the next device
// deadline, so every event is still taken by this handler at a boundary.
void grant_fast_run(State &state, const recomp_context &cpu) {
  if (state.fast_run_disabled || state.execution_fast_pending != 0U ||
      state.guest_eret_boundary ||
      state.execution_fast_deadline <= state.cpu_count + 8U)
    return;
  const std::uint64_t ticks = state.execution_fast_deadline - state.cpu_count - 4U;
  const std::uint64_t run = (std::min)(ticks << JFG_FAST_CLOCK_SHIFT, std::uint64_t{1U} << 20U);
  jfg_fast_fpu = (cpu.status_reg & 0x20000000U) != 0U ? 1U : 0U;
  jfg_fast_budget = static_cast<std::uint32_t>(run);
  state.fast_run_length = jfg_fast_budget;
}

extern "C" void jfg_phase9_execution_probe_predecoded(unsigned metadata, unsigned pc,
                                          unsigned word, void* context) {
#if defined(JFG_LIVE_RUNTIME)
  const unsigned section=metadata&255U;
  if (g_active_child_state != nullptr && context != nullptr) {
    auto& state = *g_active_child_state;
    // RGO fast clock: account the skip run that just ended.
    jfg_fast_run_end(&state);
    // Private optimization: ordinary instructions before the next event need
    // only the same cost and pipeline bookkeeping. Hardware writes, interrupt
    // eligibility, exception return and observers fall back to the full path.
    if (state.execution_fast_profile_ready &&
        !state.guest_eret_boundary && (metadata&0x800U)==0U &&
        (state.play_mode || state.observed_guest_instructions < 24000000000ULL) &&
        state.execution_fast_deadline > state.cpu_count &&
        state.execution_fast_revision == state.mmio_trace->execution_io_revision) {
      auto& cpu = *static_cast<recomp_context*>(context);
      const bool fpu=(metadata&0x100U)!=0U;
      const bool eligible = (cpu.status_reg & 7U) == 1U &&
          (state.execution_fast_pending & cpu.status_reg & 0xFF00U) != 0U;
      const bool park=(metadata&0x200U)!=0U;
      if (!eligible && !(fpu && (cpu.status_reg & 0x20000000U) == 0U) && !park) {
        ++state.observed_guest_instructions;
        ++state.execution_fast_hits;
        const bool delay = state.guest_previous_branch && pc == state.guest_last_pc + 4U;
        // RGO fast clock: a flat charge, without the cache model.
        jfg_clock_charge(&state, state.fast_clock != 0U ? 0U :
            jfg_full_cache_cost_predecoded(pc,word,&cpu,state.rdram,metadata));
        state.mmio_trace->guest_count = state.cpu_count;
        state.guest_last_pc = pc;
        state.guest_last_word = word;
        state.guest_previous_delay = delay;
        state.guest_previous_branch=(metadata&0x400U)!=0U;
        if (state.fast_clock != 0U)
          grant_fast_run(state, cpu);
        return;
      }
    }
  }
#endif
  jfg_phase9_execution_probe_slow(section,pc,word,context);
}

extern "C" __declspec(noinline) void jfg_phase9_execution_probe_slow(unsigned section, unsigned pc,
                                          unsigned word, void *context) {
#if defined(JFG_LIVE_RUNTIME)
  if (g_active_child_state != nullptr && context != nullptr) {
    auto& state = *g_active_child_state;
    jfg_fast_run_end(&state);
    if (state.eret_transfer_stream.is_open() &&
        !state.eret_transfer_probe.advance(static_cast<std::uint32_t>(state.completed_game_updates + 1U)))
      fail_closed_dispatch(state, "eret-transfer-probe", "output-or-order", pc);
    ++state.observed_guest_instructions;
    if (!state.guest_os_probe &&
        (word == 0x1000ffffU || ((word >> 26U) == 2U &&
          (((pc + 4U) & 0xf0000000U) | ((word & 0x03ffffffU) << 2U)) == pc))) {
      // Preserve the old cooperative control's permanent idle park even
      // when this root emits executable guest idle loops for the new CPU.
      pause_self(state.rdram);
    }
    if (state.guest_os_probe) {
      auto& cpu = *static_cast<recomp_context*>(context);
      auto& mmio = *state.mmio_trace;
      // A cached BEQ zero,zero,self + NOP loop has no architectural effect
      // between device events. Only skip after observing a complete warm
      // iteration and with no interrupt currently eligible for delivery.
      const auto pending_idle_interrupt =
          ((mmio.mi_pending & mmio.mi_mask.read()) ? 0x400U : 0U) |
          (state.guest_timer.interrupt() ? 0x8000U : 0U) | (state.guest_cause & 0x300U);
      if (word == 0x1000FFFFU && state.guest_last_pc == pc + 4U &&
          state.guest_last_word == 0U && state.guest_previous_delay &&
          (cpu.status_reg & 7U) == 1U &&
          (pending_idle_interrupt & cpu.status_reg & 0xFF00U) == 0U) {
        auto deadline = mmio.vi_next;
        const auto admit = [&](std::optional<std::uint64_t> next) {
          if (next) deadline = (std::min)(deadline,*next);
        };
        admit(mmio.pi_dma.deadline()); admit(mmio.si_dma.deadline());
        admit(state.guest_timer.deadline()); admit(mmio.sp_deadline);
        admit(mmio.dp_deadline); admit(mmio.ai_dma.deadline());
        if (deadline > state.cpu_count) {
          const auto ticks=deadline-state.cpu_count;
          // The fast clock keeps no cache model to warm.
          if (state.fast_clock != 0U || jfg_full_cache_idle_skip(pc,ticks,state.rdram)) {
            state.cpu_count=deadline;
            state.observed_guest_instructions+=ticks*2U;
            mmio.guest_count=state.cpu_count;
          }
        }
      }

      if (state.guest_clock_trace && mmio.traced_si_sequence != mmio.device_si_sequence) {
        state.guest_clock_trace << "si\t" << state.cpu_count << '\t' << std::hex
            << mmio.device_si_offset << '\t' << mmio.device_si_value << '\t'
            << mmio.si_dma.read(0).value_or(0) << std::dec << '\t'
            << mmio.device_si_prior_deadline.value_or(0) << '\t' << mmio.si_dma.transfers() << '\n';
        mmio.traced_si_sequence = mmio.device_si_sequence;
      }
      if (state.guest_clock_trace && (mmio.device_error ||
          mmio.traced_pi_transfers != mmio.pi_dma.transfers())) {
        state.guest_clock_trace << "pi\t" << state.cpu_count << '\t' << std::hex
            << mmio.device_pi_offset << '\t' << mmio.device_pi_value << '\t'
            << mmio.pi_dma.read(0U).value_or(0U) << '\t' << mmio.pi_dma.read(4U).value_or(0U)
            << std::dec << '\n';
        state.guest_clock_trace.flush();
        mmio.traced_pi_transfers = mmio.pi_dma.transfers();
      }
      if (state.guest_clock_trace && (pc == 0x8009ab80U || pc == 0x80098b74U))
        state.guest_clock_trace << "vi-site\t" << std::hex << pc << std::dec << '\t' << state.cpu_count << '\n';
      if (mmio.device_error != nullptr) {
        if (state.guest_clock_trace) {
          state.guest_clock_trace << "device-error\t" << mmio.device_error << '\t' << state.cpu_count
              << '\t' << std::hex << mmio.device_write_page << '\t' << mmio.device_write_offset
              << '\t' << mmio.device_write_value << std::dec << '\n';
          const auto dram = mmio.si_dma.read(0).value_or(UINT32_MAX);
          state.guest_clock_trace << "si-error\t" << std::hex << mmio.device_si_offset << '\t'
              << mmio.device_si_value << '\t' << dram << '\t';
          if (dram <= kRdramSize - 64U)
            for (unsigned i = 0; i < 64; ++i)
              state.guest_clock_trace << std::setw(2) << std::setfill('0') << unsigned(mmio.device_rdram[(dram + i) ^ 3U]);
          state.guest_clock_trace << std::dec << '\n';
          state.guest_clock_trace << "pif-error\t";
          for (const auto value : mmio.si_dma.pif())
            state.guest_clock_trace << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
          state.guest_clock_trace << std::dec << '\n';
          state.guest_clock_trace.flush();
        }
        fail_closed_dispatch(state, "guest-device", mmio.device_error, pc);
      }
      if (mmio.flash_bus.mutations() != mmio.persisted_flash_mutations) {
        if (!state.flash_path.empty() && !state.flashram.persist_atomic(state.flash_path).ok())
          fail_closed_dispatch(state, "guest-device", "flash-persist", pc);
        mmio.persisted_flash_mutations = mmio.flash_bus.mutations();
        ++state.flash_persist_count;
      }
      // No route-dependent budget or timing adjustment. Bound the diagnostic
      // until every reached device has an independently qualified owner.
      if (!state.play_mode && state.observed_guest_instructions > 24000000000ULL)
        fail_closed_dispatch(state, "guest-os", "instruction-budget", pc);
      if (mmio.sp_launch) {
        hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
        std::uint32_t flags = 0, device_type = 0;
        std::memcpy(&device_type, mmio.sp_dma.memory().data() + 0xfc0U, 4U);
        if (state.guest_active_rsp_task || state.loaded_rsp_task == 0 ||
            (state.loaded_rsp_task_type != 1 && state.loaded_rsp_task_type != 2) ||
            device_type != state.loaded_rsp_task_type ||
            !memory.read_u32(state.loaded_rsp_task + 4, flags) || (flags & ~2U) != 0)
          fail_closed_dispatch(state, "guest-rsp", "unqualified-task-launch", pc);
        state.guest_active_rsp_task = state.loaded_rsp_task;
        state.guest_active_rsp_type = device_type;
        mmio.sp_deadline = *mmio.sp_launch + (device_type == 1 ? 1000U : 4000U);
        if (device_type == 1) {
          if (mmio.dp_deadline || !state.pending_graphics_tasks.empty() ||
              !execute_live_graphics_task(state, memory))
            fail_closed_dispatch(state, "guest-rsp", "graphics-task-rejected", pc);
          mmio.dp_deadline = mmio.sp_deadline;
        }
        mmio.sp_launch.reset();
      }
      const bool device_boundary = state.guest_previous_delay || state.guest_eret_boundary;
      if (device_boundary) {
        std::array<ReferenceEventCandidate, 7> candidates{};
        std::size_t event_count = 0;
        const auto add = [&](ReferenceEvent source, std::optional<std::uint64_t> deadline) {
          if (deadline) candidates[event_count++] = {source, *deadline};
        };
        add(ReferenceEvent::vi, mmio.vi_next);
        add(ReferenceEvent::pi, mmio.pi_dma.deadline());
        add(ReferenceEvent::si, mmio.si_dma.deadline());
        add(ReferenceEvent::compare, state.guest_timer.deadline());
        add(ReferenceEvent::sp, mmio.sp_deadline);
        add(ReferenceEvent::dp, mmio.dp_deadline);
        add(ReferenceEvent::ai, mmio.ai_dma.deadline());
        const auto selected = select_reference_event(std::span(candidates).first(event_count),
            state.cpu_count, state.fast_clock != 0U);
        if (!selected.qualified) fail_closed_dispatch(state, "guest-device", "unqualified-event-tie", pc);
        std::optional<std::uint64_t> observed_deadline;
        unsigned observed_source = JFG_EVENT_NONE;
        if (state.device_event_stream && selected.source) {
          switch (*selected.source) {
          case ReferenceEvent::vi: observed_source = JFG_EVENT_VI; break;
          case ReferenceEvent::pi: observed_source = JFG_EVENT_PI; break;
          case ReferenceEvent::si: observed_source = JFG_EVENT_SI; break;
          case ReferenceEvent::compare: observed_source = JFG_EVENT_COMPARE; break;
          case ReferenceEvent::sp: observed_source = JFG_EVENT_SP; break;
          case ReferenceEvent::dp: observed_source = JFG_EVENT_DP; break;
          case ReferenceEvent::ai: observed_source = JFG_EVENT_AI; break;
          }
          for (const auto candidate : std::span(candidates).first(event_count))
            if (candidate.source == *selected.source) observed_deadline = candidate.deadline;
          emit_device_event(state, cpu, JFG_EVENT_DISPATCH, observed_source, pc, word, observed_deadline);
        }
        if (selected.source) switch (*selected.source) {
        case ReferenceEvent::vi:
          mmio.mi_pending |= 8U;
          ++state.vi_frames;
          // Host playback follows the original VI device clock. This does not
          // advance Count, invent a guest event, or adjust a cinematic cue.
          if (state.original_os_host_pacing && state.host_audio.started()) {
            if (!state.original_os_host_anchor_ready) {
              state.host_audio.settle_startup_queue();
              state.original_os_host_clock.anchor(mmio.vi_next,
                  std::chrono::steady_clock::now(), state.host_audio.underruns());
              state.original_os_host_anchor_ready = true;
            }
            // Poll actual output continuity before pacing, including stalls in
            // the window/renderer owner thread. Never catch up a stale clock
            // after the audio device has run dry.
            state.host_audio.service(false, false);
            auto target_time = state.original_os_host_clock.deadline(
                mmio.vi_next, std::chrono::steady_clock::now(),
                state.host_audio.underruns());
            // RGO: hold the audio queue at its cushion (QueueLevelTrim). Only
            // run ahead when this VI would wait anyway, so a host that is
            // behind does not build up a debt it later races through.
            if (state.audio_level_trim) {
              const std::int64_t trim = state.host_audio.level_trim(
                  static_cast<std::int64_t>(mmio.vi_period) * 64 / 3);
              const auto earlier = std::chrono::nanoseconds(trim);
              if (trim < 0 || target_time - std::chrono::steady_clock::now() > earlier) {
                state.original_os_host_clock.trim(earlier);
                target_time -= earlier;
                state.audio_trim_ns += trim;
              }
            }
            if (!state.unpaced && std::chrono::steady_clock::now() < target_time)
              paced_sleep_until(state, target_time);
            // RGO fast clock: this VI's Count is due at this wall time.
            state.wall_anchor_count = mmio.vi_next;
            state.wall_anchor_time = target_time;
            state.wall_anchor_valid = true;
            const auto now = std::chrono::steady_clock::now();
            if (state.timing_trace) state.timing_trace << "os-host-vi\t"
                << state.vi_frames << '\t' << mmio.vi_next << '\t'
                << std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()
                << '\t' << std::chrono::duration_cast<std::chrono::microseconds>(now-target_time).count()
                << '\n';
          }
          if (state.timing_trace) {
            hle::GuestMemory observed({state.rdram,kRdramSize},hle::GuestMemory::Layout::native_word_big_endian);
            std::uint32_t loading=0;
            if (observed.read_u32(0x800A3530U,loading) && loading!=0U) {
              std::uint16_t level=0,next=0;
              std::memcpy(&level,state.rdram+(0xA323CU^2U),sizeof(level));
              std::memcpy(&next,state.rdram+(0xA3250U^2U),sizeof(next));
              const auto original_address=[&](std::uint32_t value) {
                if(value<0x00100000U || value>=0x10000000U)return value;
                std::uint32_t table=0,base=0;
                if (!observed.read_u32(0x800FEAA0U,table) ||
                    !observed.read_u32(table+(value>>20U)*32U,base))return 0U;
                return base+(value&0xFFFFFU);
              };
              state.timing_trace << "load-pc\t" << state.vi_frames << '\t'
                  << loading << '\t' << level << '\t' << next << '\t'
                  << std::hex << original_address(pc) << '\t'
                  << original_address(static_cast<std::uint32_t>(cpu.r31)) << '\t'
                  << static_cast<std::uint32_t>(cpu.r29) << '\t'
                  << mmio.sp_status.read() << '\t' << mmio.pi_dma.read(0x10U).value_or(0U)
                  << std::dec << '\n';
            }
          }

          jfg::support_breadcrumb(state.vi_frames, state.vi_retraces, state.controller_samples);
          if (mmio.vi_v_sync != 0U && mmio.vi_v_sync != 525U)
            fail_closed_dispatch(state, "guest-os", "unqualified-vi-mode", pc);
          if (mmio.vi_v_sync != 0U && mmio.vi_h_sync != 3093U)
            fail_closed_dispatch(state, "guest-os", "unqualified-hardware-hsync", pc);
          // Same independently derived fixed progressive profile as the paired
          // Ares comparison; this does not implement arbitrary VI register modes.
          mmio.vi_period = mmio.vi_v_sync == 0U ? 500000U : 783520U;
          mmio.vi_next += mmio.vi_period;
          if (!service_host_audio(state, true))
            fail_closed_dispatch(state, "audio", "host-device", pc);
          break;
        case ReferenceEvent::pi: mmio.pi_dma.advance(state.cpu_count); break;
        case ReferenceEvent::si: mmio.si_dma.advance(state.cpu_count); break;
        case ReferenceEvent::compare: state.guest_timer.advance(state.cpu_count); break;
        case ReferenceEvent::ai: mmio.ai_dma.advance(state.cpu_count); break;
        case ReferenceEvent::sp: {
          if (state.guest_active_rsp_type == 1) complete_pending_live_graphics_tasks(state, pc);
          else if (state.guest_active_rsp_type == 2) {
            hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
            const auto loaded = state.loaded_rsp_task;
            state.loaded_rsp_task = state.guest_active_rsp_task;
            if (!execute_live_audio_task(state, memory))
              fail_closed_dispatch(state, "guest-rsp", "audio-task-rejected", pc);
            state.loaded_rsp_task = loaded;
          } else fail_closed_dispatch(state, "guest-rsp", "completion-without-task", pc);
          if (!mmio.sp_status.complete_task()) fail_closed_dispatch(state, "guest-rsp", "completion-status", pc);
          mmio.sp_deadline.reset();
          state.guest_active_rsp_task = state.guest_active_rsp_type = 0;
          break;
        }
        case ReferenceEvent::dp: mmio.mi_pending |= 32U; mmio.dp_deadline.reset(); break;
        }
        mmio.mi_pending = (mmio.mi_pending & ~1U) | (mmio.sp_status.interrupt() ? 1U : 0U);
        mmio.mi_pending = (mmio.mi_pending & ~2U) | (mmio.si_dma.interrupt() ? 2U : 0U);
        mmio.mi_pending = (mmio.mi_pending & ~4U) | (mmio.ai_dma.interrupt() ? 4U : 0U);
        mmio.mi_pending = (mmio.mi_pending & ~16U) | (mmio.pi_dma.interrupt() ? 16U : 0U);
        if (observed_source != JFG_EVENT_NONE)
          emit_device_event(state, cpu, JFG_EVENT_STATE, observed_source, pc, word, observed_deadline);
      }
      state.vi_interrupts = mmio.vi_acknowledgements;
      bool delay = state.guest_previous_branch && pc == state.guest_last_pc + 4U;
      const auto cpu_pending = ((mmio.mi_pending & mmio.mi_mask.read()) ? 0x400U : 0U) |
          (state.guest_timer.interrupt() ? 0x8000U : 0U) | (state.guest_cause & 0x300U);
      const bool external = (cpu_pending & cpu.status_reg & 0xff00U) != 0U &&
          (cpu.status_reg & 7U) == 1U &&
          (device_boundary || (state.guest_last_word & 0xffe0ffffU) == 0x40806000U);
      state.guest_eret_boundary = false;
      const unsigned primary = word >> 26U;
      const bool fpu = primary == 0x11U || primary == 0x31U || primary == 0x35U || primary == 0x39U || primary == 0x3dU;
      const bool unusable = fpu && (cpu.status_reg & 0x20000000U) == 0U;
      if (external || unusable) {
        if (external)
          emit_device_event(state, cpu, JFG_EVENT_ACCEPT, JFG_EVENT_NONE, pc, word);
        if ((cpu.status_reg & 2U) != 0U || (unusable && delay))
          fail_closed_dispatch(state, "guest-os", "unqualified-exception-boundary", pc);
        const auto interrupted_pc = pc;
        state.guest_transport->park_at(pc);
        state.guest_epc = pc;
        state.guest_cause &= ~0x8000007cU;
        if (unusable) state.guest_cause = (state.guest_cause & ~0x30000000U) | 0x1000002cU;
        cop0_status_write(&cpu, cpu.status_reg | 2U);
        ++state.guest_exceptions;
        state.guest_previous_branch = state.guest_previous_delay = false;
        hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
        for (std::uint32_t offset = 0U; offset < 16U; offset += 4U) {
          std::uint32_t source = 0U, installed = 0U;
          if (!memory.read_u32(0x80075020U + offset, source) ||
              !memory.read_u32(0x80000180U + offset, installed) || source != installed)
            fail_closed_dispatch(state, "guest-os", "exception-vector-mismatch", pc);
        }
        if (state.guest_clock_trace.is_open()) {
          state.guest_clock_trace << "exception\t" << std::hex << pc << '\t' << mmio.mi_pending
              << '\t' << state.guest_cause << std::dec << '\t' << state.cpu_count << '\n';
          state.guest_clock_trace.flush();
        }
        const auto handler = jfg_generated_lookup_function(static_cast<std::int32_t>(0x80075020U));
        if (handler == nullptr) fail_closed_dispatch(state, "guest-os", "exception-handler-unavailable", pc);
        handler(state.rdram, &cpu);
        if (state.guest_epc != interrupted_pc || (cpu.status_reg & 2U) != 0U)
          fail_closed_dispatch(state, "guest-os", "exception-resume-mismatch", pc);
        delay = false;
      }
      if (state.device_event_stream && !state.device_event_probe.complete) {
        const auto invocation = state.completed_game_updates + 1U;
        if (invocation > state.device_event_probe.last) {
          if (!jfg_device_event_finish(&state.device_event_probe))
            fail_closed_dispatch(state, "device-event-probe", "output-failed", pc);
        } else if (invocation >= state.device_event_probe.first) {
          hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
          std::uint32_t thread = 0U;
          if (!memory.read_u32(0x800a9e90U, thread))
            fail_closed_dispatch(state, "device-event-probe", "thread-read", pc);
          if (thread != state.device_event_previous_thread)
            emit_device_event(state, cpu, JFG_EVENT_THREAD, JFG_EVENT_NONE, pc, word);
          if (state.device_event_successor)
            emit_device_event(state, cpu, JFG_EVENT_SUCCESSOR, JFG_EVENT_NONE, pc, word);
          const bool selected_pc = std::find(state.point_probe_pcs.begin(), state.point_probe_pcs.end(), pc) != state.point_probe_pcs.end();
          if (selected_pc) emit_device_event(state, cpu, JFG_EVENT_INSTRUCTION, JFG_EVENT_NONE, pc, word);
          state.device_event_successor = selected_pc;
          state.device_event_previous_thread = thread;
          state.device_event_previous_pc = pc;
          state.device_event_previous_word = word;
        }
      }
      // Mupen's annulled ordinary likely slots still advance Count. Other
      // likely families require their own profile coverage before use.
      jfg_clock_charge(&state, state.fast_clock != 0U ? 0U :
          jfg_full_cache_cost(pc,word,&cpu,state.rdram));
      mmio.guest_count = state.cpu_count;
      state.guest_last_pc = pc;
      state.guest_last_word = word;
      state.guest_previous_delay = delay;
      state.guest_previous_branch = primary == 1U || (primary >= 2U && primary <= 7U) ||
          (primary >= 20U && primary <= 23U) ||
          (primary == 0U && ((word & 63U) == 8U || (word & 63U) == 9U)) ||
          (primary == 0x11U && ((word >> 21U) & 31U) == 8U);
      if (primary == 3U && (((pc + 4U) & 0xf0000000U) | ((word & 0x03ffffffU) << 2U)) == 0x80075698U)
        state.guest_transport->park_at(pc + 8U);
      state.execution_fast_deadline = mmio.vi_next;
      const auto fast_admit = [&](std::optional<std::uint64_t> next) {
        if (next) state.execution_fast_deadline =
            (std::min)(state.execution_fast_deadline,*next);
      };
      fast_admit(mmio.pi_dma.deadline()); fast_admit(mmio.si_dma.deadline());
      fast_admit(state.guest_timer.deadline()); fast_admit(mmio.sp_deadline);
      fast_admit(mmio.dp_deadline); fast_admit(mmio.ai_dma.deadline());
      state.execution_fast_revision = mmio.execution_io_revision;
      state.execution_fast_pending =
          ((mmio.mi_pending & mmio.mi_mask.read()) ? 0x400U : 0U) |
          (state.guest_timer.interrupt() ? 0x8000U : 0U) | (state.guest_cause & 0x300U);

      // After exception resumption and Count accounting, before the current
      // guest instruction. No clock reads/changes or guest writes in this hook.
      // Raw Count is deliberately omitted: the oracle exposes lazy core Count.
      if (!state.point_probe_pcs.empty() &&
          state.completed_game_updates + 1U >= state.focus_update_start - 1U &&
          state.completed_game_updates + 1U <= state.focus_update_end + 1U &&
          std::find(state.point_probe_pcs.begin(), state.point_probe_pcs.end(), pc) != state.point_probe_pcs.end()) {
        if (++state.point_probe_hits > 4096U)
          fail_closed_dispatch(state, "point-probe", "hit-budget", pc);
        hle::GuestMemory memory({state.rdram, kRdramSize}, hle::GuestMemory::Layout::native_word_big_endian);
        std::uint32_t opcode = 0;
        if (!memory.read_u32(pc, opcode) || opcode != word)
          fail_closed_dispatch(state, "point-probe", "instruction-bytes", pc);
        auto& out = state.point_probe_trace;
        out << state.completed_game_updates + 1U << '\t' << state.vi_retraces << '\t'
            << state.controller_samples << '\t' << "0x" << std::hex << std::setfill('0')
            << std::setw(8) << pc << '\t' << "0x" << std::setw(8) << opcode;
        const std::array<std::uint64_t, 32> registers = {
            0, cpu.r1, cpu.r2, cpu.r3, cpu.r4, cpu.r5, cpu.r6, cpu.r7,
            cpu.r8, cpu.r9, cpu.r10, cpu.r11, cpu.r12, cpu.r13, cpu.r14, cpu.r15,
            cpu.r16, cpu.r17, cpu.r18, cpu.r19, cpu.r20, cpu.r21, cpu.r22, cpu.r23,
            cpu.r24, cpu.r25, cpu.r26, cpu.r27, cpu.r28, cpu.r29, cpu.r30, cpu.r31};
        for (const auto value : registers)
          out << '\t' << "0x" << std::setw(8) << static_cast<std::uint32_t>(value)
              << '\t' << "0x" << std::setw(8) << static_cast<std::uint32_t>(value >> 32U);
        for (const auto address : state.point_probe_words) {
          std::uint32_t value = 0;
          if (!memory.read_u32(address, value))
            fail_closed_dispatch(state, "point-probe", "word-read", pc);
          out << '\t' << "0x" << std::setw(8) << value;
        }
        out << std::dec << std::setfill(' ') << '\n';
        out.flush();
        if (!out) fail_closed_dispatch(state, "point-probe", "output-failed", pc);
      }
    }
  }
#endif
  (void)section; (void)pc; (void)word; (void)context;
}

bool read_supported_rom(const char *path, std::vector<std::uint8_t> &rom) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() != static_cast<std::streamoff>(kRomSize))
    return false;
  input.seekg(0);
  rom.resize(kRomSize);
  return input.read(reinterpret_cast<char *>(rom.data()),
                    static_cast<std::streamsize>(rom.size())) &&
         sha1(rom) == kSupportedRomSha1;
}

int run_child(const char *path, const unsigned retrace_target,
              const unsigned poll_target, const bool play_mode) {
  std::vector<std::uint8_t> rom;
  if (!read_supported_rom(path, rom))
    return 2;
  if constexpr (std::endian::native != std::endian::little) {
    std::fputs("native boot setup failed: unsupported host endianness\n",
               stderr);
    return 3;
  }
  std::vector<std::uint8_t> bytes(kRdramSize);
  GuestBacking backing;
  if (!backing.create() || !backing.load_overlay_images(rom)) {
    std::fputs("native boot setup failed: guest address space\n", stderr);
    return 3;
  }
  const std::span<std::uint8_t> rdram = backing.bytes();
  if (!load_static_sections(rom, bytes)) {
    std::fputs("native boot setup failed: static section validation\n", stderr);
    return 3;
  }
  const auto handoff = interpret_reset_handoff(
      bytes, 0x80000400U, kRdramSize / sizeof(std::uint32_t) + 4096U);
  if (!handoff.ok()) {
    const char *reason =
        handoff.error == ResetHandoffError::unsupported_instruction
            ? "unsupported-instruction"
        : handoff.error == ResetHandoffError::budget_exhausted    ? "budget"
        : handoff.error == ResetHandoffError::unaligned_access    ? "unaligned"
        : handoff.error == ResetHandoffError::arithmetic_overflow ? "overflow"
                                                                  : "memory";
    std::fprintf(stderr, "native boot setup failed: reset handoff %s\n",
                 reason);
    return 3;
  }
  if (!initialize_ipl_state(bytes, rom)) {
    std::fputs("native boot setup failed: IPL state\n", stderr);
    return 3;
  }
  for (std::size_t index = 0U; index < rdram.size(); index += 4U) {
    rdram[index] = bytes[index + 3U];
    rdram[index + 1U] = bytes[index + 2U];
    rdram[index + 2U] = bytes[index + 1U];
    rdram[index + 3U] = bytes[index];
  }
  jfg::GeneratedOverlayRuntime runtime(std::as_writable_bytes(rdram));
  State state{};
#if defined(JFG_LIVE_RUNTIME)
  // RGO host speed switches (docs/rgo/framerate.md, "Host speed"): =1 turns
  // each on, on Windows and in the headless build.
  {
    const auto switch_on = [](const char *name) {
      char flag[2]{};
      return GetEnvironmentVariableA(name, flag, 2U) == 1U && flag[0] == '1';
    };
    state.present_full_import = switch_on("JFG_RGO_PRESENT_FULL_IMPORT");
    state.snapshot_full_import = switch_on("JFG_RGO_SNAPSHOT_FULL");
    state.snapshot_verify = switch_on("JFG_RGO_SNAPSHOT_VERIFY");
    {
      char flag[2]{};
      state.audio_level_trim = !(GetEnvironmentVariableA("JFG_RGO_AUDIO_TRIM", flag, 2U) == 1U &&
                                 flag[0] == '0');
    }
  }
  char mod_flag[2]{};
  if (GetEnvironmentVariableA("JFG_NAVIGATION_MOD",mod_flag,2U)==1U && mod_flag[0]=='1') {
    wchar_t output[32768]{};
    const DWORD length=GetEnvironmentVariableW(L"JFG_MOD_OUTPUT",output,32768U);
    if(length==0U || length>=32768U) {
      std::fputs("navigation mod requires JFG_MOD_OUTPUT\n",stderr);return 2;
    }
    state.navigation_output=std::filesystem::path(output);
    std::error_code error;std::filesystem::create_directories(state.navigation_output,error);
    if(error)return 2;
    state.navigation_mod.enabled=true;
    // File open/flush stays on the diagnostic worker, never the guest thread.
    (void)state.movement_trace.start(state.navigation_output/L"movement-samples.csv", 4U*1024U*1024U);
    (void)state.jump_trace.start(state.navigation_output/L"jump-samples.csv", 4U*1024U*1024U);
    jfg::support_event("mod=navigation-enabled");
    char speed_flag[2]{};
    state.navigation_mod.juno_double_run =
        GetEnvironmentVariableA("JFG_MOD_JUNO_DOUBLE_RUN", speed_flag, 2U) == 1U &&
        speed_flag[0] == '1';
    if (state.navigation_mod.juno_double_run)
      jfg::support_event("mod=juno-run-speed-2x-enabled");
  }
  // Live tools observe by default; map Mods explicitly opt into individual changes.
  if (!state.navigation_mod.enabled) {
    wchar_t output[32768]{};
    const DWORD length=GetEnvironmentVariableW(L"JFG_LIVE_OUTPUT",output,32768U);
    if (length != 0U && length < 32768U) {
      state.navigation_output=std::filesystem::path(output);
      std::error_code error;std::filesystem::create_directories(state.navigation_output,error);
      if (!error) state.navigation_mod.telemetry_enabled=true;
    }
  }
  wchar_t gameplay_trace_path[32768]{};
  const DWORD trace_path_length = GetEnvironmentVariableW(L"JFG_GAMEPLAY_TRACE", gameplay_trace_path, 32768U);
  if (trace_path_length != 0U && (trace_path_length >= 32768U ||
      !state.gameplay_trace.open(std::filesystem::path(gameplay_trace_path)))) {
    std::fputs("native boot setup failed: gameplay trace path\n", stderr);
    return 3;
  }
  state.host_audio.set_gameplay_trace(&state.gameplay_trace);
  state.host_audio.set_vi_counter(&state.vi_retraces);
  g_active_child_state = &state;
  jfg_original_timing_state = &state;
  state.instruction_cache = ic; state.data_cache = dc; state.cache_counters = cache_stats;
  jfg::support_event("native=rom-ready");
  wchar_t mapping_path[32768]{};
  const DWORD mapping_length = GetEnvironmentVariableW(L"JFG_CONTROLLER_CONFIG", mapping_path, 32768U);
  if (mapping_length != 0U) {
    state.controller_config_path=std::filesystem::path(mapping_path);
    std::ifstream mapping_file(std::filesystem::path(mapping_path), std::ios::binary | std::ios::ate);
    if (mapping_length >= 32768U || !mapping_file || mapping_file.tellg() > 4096 || mapping_file.tellg() <= 0) {
      jfg::support_event("native=controller-invalid");
      return 2;
    }
    mapping_file.seekg(0);
    const std::string mapping_text{std::istreambuf_iterator<char>(mapping_file), std::istreambuf_iterator<char>()};
    if (!jfg::parse_controller_mapping(mapping_text, state.controller_mapping)) {
      jfg::support_event("native=controller-invalid");
      return 2;
    }
  }
  state.controller_ports.configure(0,state.controller_mapping);
  sample_controller_ports(state);
  // One opt-in runtime mode owns CPU, OS, device clocks and host pacing.
  // Diagnostic traces and the travel fixture are independent and optional.
#if defined(JFG_HEADLESS_LIVE)
  // RGO frame trace (headless measurement, docs/rgo/framerate.md).
  if (const char *trace = std::getenv("JFG_RGO_FRAME_TRACE");
      trace != nullptr && *trace != '\0') {
    state.rgo_frame_trace = std::fopen(trace, "w");
    if (state.rgo_frame_trace != nullptr)
      std::setvbuf(state.rgo_frame_trace, nullptr, _IOLBF, 1U << 16U);
    if (state.rgo_frame_trace == nullptr) {
      std::fputs("native boot setup failed: frame trace path\n", stderr);
      return 3;
    }
    std::fputs("vi\tticks\tdelta_time\tcount\tinstructions\twall_us\tfront_mode\tlevel\t"
               "busy_us\ttrue60\taudio_underruns\taudio_queued_ms\tfloor_jumps\tfloor_resets\taudio_trim_us\n",
               state.rgo_frame_trace);
  }
  state.counter_log = std::getenv("JFG_RGO_COUNTER_LOG") != nullptr;
  state.wall_floor_disabled = std::getenv("JFG_RGO_DETERMINISTIC_CLOCK") != nullptr;
  state.fast_run_disabled = std::getenv("JFG_RGO_NO_FAST_RUNS") != nullptr;
  state.unpaced = state.wall_floor_disabled && std::getenv("JFG_RGO_UNPACED") != nullptr;
  if (const char *pilot = std::getenv("JFG_RGO_AUTOPILOT");
      pilot != nullptr && std::strcmp(pilot, "newgame") == 0) {
    state.autopilot.emplace();
    // The game must find a controller in port 1 when it boots.
    state.controller_ports.samples[0].connected = true;
    for (std::uint32_t section = 0U; section < jfg_generated_section_count(); ++section) {
      JfgGeneratedSectionMetadata metadata{};
      if (jfg_generated_section_metadata(section, &metadata) != 0 && metadata.is_overlay == 1U)
        state.autopilot_overlays.emplace_back(metadata.linked_vram,
            metadata.linked_vram + metadata.text_size + metadata.data_size + metadata.bss_size);
    }
  }
#endif
  char timing_fixture_flag[2]{};
  state.timing_fixture = GetEnvironmentVariableA("JFG_TIMING_TEST_FIXTURE",timing_fixture_flag,2U)==1U &&
      timing_fixture_flag[0]=='1';
  char original_timing_flag[2]{};
  const auto timing_override = GetEnvironmentVariableA("JFG_ORIGINAL_TIMING", original_timing_flag, 2U);
  const bool original_timing_mode = timing_override == 1U ? original_timing_flag[0] == '1' :
      JFG_ORIGINAL_TIMING_GENERATION == 1;
  if (original_timing_mode && JFG_ORIGINAL_TIMING_GENERATION != 1 && timing_override != 1U)
    return 3;
  char writeback_flag[2]{};
  state.renderer_writeback_probe = original_timing_mode || (
      GetEnvironmentVariableA("JFG_PHASE9_RENDERER_WRITEBACK_PROBE", writeback_flag, 2U) == 1U &&
      writeback_flag[0] == '1');
  char leaf_flag[2]{};
  state.guest_leaf_probe = original_timing_mode || (
      GetEnvironmentVariableA("JFG_PHASE9_GUEST_LEAF_PROBE", leaf_flag, 2U) == 1U &&
      leaf_flag[0] == '1');
  char os_flag[2]{};
  state.guest_os_probe = original_timing_mode || (
      GetEnvironmentVariableA("JFG_PHASE9_GUEST_OS_PROBE", os_flag, 2U) == 1U && os_flag[0] == '1');
  char original_pacing_flag[2]{};
  state.original_os_host_pacing = original_timing_mode || (
      GetEnvironmentVariableA("JFG_DIAGNOSTIC_OS_VI_PACING", original_pacing_flag, 2U) == 1U &&
      original_pacing_flag[0] == '1');
  if (state.original_os_host_pacing && !state.guest_os_probe) return 3;
  if (state.guest_os_probe && !state.guest_leaf_probe) {
    std::fputs("native boot setup failed: guest OS probe requires the guest CPU root\n", stderr);
    return 3;
  }
  if (state.guest_leaf_probe)
    state.tlb_registers = jfg::boot::TlbRegisters32::observed_mupen_boot();
#if defined(_WIN32)
  (void)SetUnhandledExceptionFilter(child_exception_filter);
#else
  g_crash_report = headless_crash_report;
#endif
#endif
  state.retrace_target =
      play_mode || poll_target != 0U
          ? (std::numeric_limits<std::uint32_t>::max)()
          : retrace_target;
#if defined(JFG_HEADLESS_LIVE)
  // Headless measurement: end a --play run after this many VIs.
  if (const char *limit = std::getenv("JFG_RGO_EXIT_AFTER_VI");
      play_mode && limit != nullptr && *limit != '\0')
    state.retrace_target = static_cast<std::uint32_t>(std::strtoul(limit, nullptr, 10));
#endif
  state.poll_target = poll_target;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  state.play_mode = play_mode;
#if defined(_WIN32)
  char *fast_replay = nullptr;
  std::size_t fast_replay_size = 0U;
  if (_dupenv_s(&fast_replay, &fast_replay_size,
                "JFG_PHASE9_FAST_REPLAY") != 0) {
    std::fputs("native boot setup failed: fast replay environment\n",
               stderr);
    return 3;
  }
  if (fast_replay != nullptr && *fast_replay != '\0') {
    if (std::strcmp(fast_replay, "1") != 0) {
      std::free(fast_replay);
      std::fputs("native boot setup failed: fast replay value\n", stderr);
      return 3;
    }
    state.fast_replay = true;
  }
  std::free(fast_replay);
  char *capture = nullptr;
  std::size_t capture_size = 0U;
  if (_dupenv_s(&capture, &capture_size, "JFG_PHASE8_FRAME_CAPTURE") != 0) {
    std::fputs("native boot setup failed: frame capture environment\n",
               stderr);
    return 3;
  }
  if (capture != nullptr && *capture != '\0') {
    if (capture_size > 32768U) {
      std::free(capture);
      std::fputs("native boot setup failed: frame capture path\n", stderr);
      return 3;
    }
    state.frame_capture_path = capture;
  }
  std::free(capture);
  char *pcm_capture = nullptr;
  std::size_t pcm_capture_size = 0U;
  if (_dupenv_s(&pcm_capture, &pcm_capture_size,
                "JFG_PHASE8_PCM_CAPTURE") != 0) {
    std::fputs("native boot setup failed: PCM capture environment\n",
               stderr);
    return 3;
  }
  if (pcm_capture != nullptr && *pcm_capture != '\0' &&
      (pcm_capture_size > 32768U ||
       !state.host_audio.configure_capture(pcm_capture))) {
    std::free(pcm_capture);
    std::fputs("native boot setup failed: PCM capture path\n", stderr);
    return 3;
  }
  std::free(pcm_capture);
  char *rdram_capture = nullptr;
  std::size_t rdram_capture_size = 0U;
  if (_dupenv_s(&rdram_capture, &rdram_capture_size,
                "JFG_PHASE8_RDRAM_CAPTURE") != 0) {
    std::fputs("native boot setup failed: RDRAM capture environment\n",
               stderr);
    return 3;
  }
  if (rdram_capture != nullptr && *rdram_capture != '\0') {
    if (rdram_capture_size > 32768U) {
      std::free(rdram_capture);
      std::fputs("native boot setup failed: RDRAM capture path\n", stderr);
      return 3;
    }
    state.rdram_capture_path = rdram_capture;
  }
  std::free(rdram_capture);
  char *progress = nullptr;
  std::size_t progress_size = 0U;
  if (_dupenv_s(&progress, &progress_size, "JFG_PHASE8_PROGRESS") != 0) {
    std::fputs("native boot setup failed: progress environment\n", stderr);
    return 3;
  }
  if (progress != nullptr && *progress != '\0') {
    if (progress_size > 32768U) {
      std::free(progress);
      std::fputs("native boot setup failed: progress path\n", stderr);
      return 3;
    }
    state.progress_path = progress;
  }
  std::free(progress);
  char *record_path = nullptr;
  std::size_t record_path_size = 0U;
  if (_dupenv_s(&record_path, &record_path_size,
                "JFG_PHASE9_INPUT_RECORD") != 0) {
    std::fputs("native boot setup failed: input record environment\n", stderr);
    return 3;
  }
  if (record_path != nullptr && *record_path != '\0') {
    if (record_path_size > 32768U) {
      std::free(record_path);
      std::fputs("native boot setup failed: input record path\n", stderr);
      return 3;
    }
    state.input_record_path = record_path;
  }
  std::free(record_path);
  char *actor_trace = nullptr;
  std::size_t actor_trace_size = 0U;
  if (_dupenv_s(&actor_trace, &actor_trace_size,
                "JFG_PHASE9_ACTOR_TRACE") != 0) {
    std::fputs("native boot setup failed: actor trace environment\n", stderr);
    return 3;
  }
  if (actor_trace != nullptr && *actor_trace != '\0') {
    if (std::strcmp(actor_trace, "1") != 0) {
      std::free(actor_trace);
      std::fputs("native boot setup failed: actor trace value\n", stderr);
      return 3;
    }
    state.actor_timing_trace = true;
  }
  std::free(actor_trace);
  char *retrace_hash = nullptr;
  std::size_t retrace_hash_size = 0U;
  if (_dupenv_s(&retrace_hash, &retrace_hash_size,
                "JFG_PHASE9_RETRACE_HASH") != 0) {
    std::fputs("native boot setup failed: retrace hash environment\n",
               stderr);
    return 3;
  }
  if (retrace_hash != nullptr && *retrace_hash != '\0') {
    if (retrace_hash_size > 32768U) {
      std::free(retrace_hash);
      std::fputs("native boot setup failed: retrace hash path\n", stderr);
      return 3;
    }
    state.retrace_hash_path = retrace_hash;
  }
  std::free(retrace_hash);
  char *replay_by_poll = nullptr;
  std::size_t replay_by_poll_size = 0U;
  if (_dupenv_s(&replay_by_poll, &replay_by_poll_size,
                "JFG_PHASE9_REPLAY_BY_POLL") != 0) {
    std::fputs("native boot setup failed: replay poll environment\n", stderr);
    return 3;
  }
  if (replay_by_poll != nullptr && *replay_by_poll != '\0') {
    if (std::strcmp(replay_by_poll, "1") != 0) {
      std::free(replay_by_poll);
      std::fputs("native boot setup failed: replay poll value\n", stderr);
      return 3;
    }
    state.input_replay_by_poll = true;
  }
  std::free(replay_by_poll);
  char *replay_path = nullptr;
  std::size_t replay_path_size = 0U;
  if (_dupenv_s(&replay_path, &replay_path_size,
                "JFG_PHASE8_INPUT_REPLAY") != 0) {
    std::fputs("native boot setup failed: input replay environment\n",
               stderr);
    return 3;
  }
  if (replay_path != nullptr && *replay_path != '\0') {
    if (replay_path_size > 32768U ||
        jfg::DeterministicInputReplay::load(replay_path,
                                             state.input_replay, state.input_replay_by_poll
                  ? jfg::InputReplayOrder::recorded_poll
                  : jfg::InputReplayOrder::retrace) !=
            jfg::InputReplayError::none) {
      std::free(replay_path);
      std::fputs("native boot setup failed: input replay\n", stderr);
      return 3;
    }
    state.input_replay_loaded = true;
    state.input_replay_event_count = state.input_replay.events().size();
  }
  std::free(replay_path);
  if (state.input_replay_by_poll && !state.input_replay_loaded) {
    std::fputs("native boot setup failed: replay poll requires input replay\n", stderr);
    return 3;
  }
  // A bounded, hidden replay can exercise the interactive VI/audio cadence
  // without opening another game window or consuming live controller input.
  char *realtime_replay = nullptr;
  std::size_t realtime_replay_size = 0U;
  if (_dupenv_s(&realtime_replay, &realtime_replay_size,
                "JFG_PHASE9_REALTIME_REPLAY") != 0) {
    std::fputs("native boot setup failed: realtime replay environment\n", stderr);
    return 3;
  }
  if (realtime_replay != nullptr && *realtime_replay != '\0') {
    if (std::strcmp(realtime_replay, "1") != 0 ||
        !state.input_replay_loaded || state.play_mode || state.fast_replay ||
        state.guest_leaf_probe) {
      std::free(realtime_replay);
      std::fputs("native boot setup failed: realtime replay value\n", stderr);
      return 3;
    }
    state.realtime_replay = true;
    jfg::support_event("replay=hidden-realtime-vi-clock");
  }
  std::free(realtime_replay);
  if (state.poll_target != 0U &&
      (!state.input_replay_loaded || !state.input_replay_by_poll)) {
    std::fputs("native boot setup failed: poll target requires replay-by-poll\n",
               stderr);
    return 3;
  }
  char *save_path = nullptr;
  std::size_t save_path_size = 0U;
  if (_dupenv_s(&save_path, &save_path_size, "JFG_PHASE8_SAVE_PATH") != 0) {
    std::fputs("native boot setup failed: save environment\n", stderr);
    return 3;
  }
  if (save_path != nullptr && *save_path != '\0' &&
      (save_path_size > 32768U || !configure_flash_path(state, save_path))) {
    std::free(save_path);
    std::fputs("native boot setup failed: save path\n", stderr);
    return 3;
  }
  std::free(save_path);
  char *controller_pak_path = nullptr;
  std::size_t controller_pak_path_size = 0U;
  if (_dupenv_s(&controller_pak_path, &controller_pak_path_size,
                "JFG_PHASE8_CONTROLLER_PAK_PATH") != 0) {
    std::fputs("native boot setup failed: controller pak environment\n",
               stderr);
    return 3;
  }
  if (controller_pak_path != nullptr && *controller_pak_path != '\0' &&
      (controller_pak_path_size > 32768U ||
       !configure_controller_pak_path(state, controller_pak_path))) {
    std::free(controller_pak_path);
    std::fputs("native boot setup failed: controller pak path\n", stderr);
    return 3;
  }
  std::free(controller_pak_path);
#else
  if (const char *fast_replay = std::getenv("JFG_PHASE9_FAST_REPLAY");
      fast_replay != nullptr && *fast_replay != '\0') {
    if (std::strcmp(fast_replay, "1") != 0) {
      std::fputs("native boot setup failed: fast replay value\n", stderr);
      return 3;
    }
    state.fast_replay = true;
  }
  if (const char *capture = std::getenv("JFG_PHASE8_FRAME_CAPTURE");
      capture != nullptr && *capture != '\0') {
    if (std::strlen(capture) > 32767U) {
      std::fputs("native boot setup failed: frame capture path\n", stderr);
      return 3;
    }
    state.frame_capture_path = capture;
  }
  if (const char *rdram_capture = std::getenv("JFG_PHASE8_RDRAM_CAPTURE");
      rdram_capture != nullptr && *rdram_capture != '\0') {
    if (std::strlen(rdram_capture) > 32767U) {
      std::fputs("native boot setup failed: RDRAM capture path\n", stderr);
      return 3;
    }
    state.rdram_capture_path = rdram_capture;
  }
  if (const char *progress = std::getenv("JFG_PHASE8_PROGRESS");
      progress != nullptr && *progress != '\0') {
    if (std::strlen(progress) > 32767U) {
      std::fputs("native boot setup failed: progress path\n", stderr);
      return 3;
    }
    state.progress_path = progress;
  }
  if (const char *retrace_hash = std::getenv("JFG_PHASE9_RETRACE_HASH");
      retrace_hash != nullptr && *retrace_hash != '\0') {
    if (std::strlen(retrace_hash) > 32767U) {
      std::fputs("native boot setup failed: retrace hash path\n", stderr);
      return 3;
    }
    state.retrace_hash_path = retrace_hash;
  }
  if (const char *replay_path = std::getenv("JFG_PHASE8_INPUT_REPLAY");
      replay_path != nullptr && *replay_path != '\0') {
    if (std::strlen(replay_path) > 32767U ||
        jfg::DeterministicInputReplay::load(replay_path,
                                             state.input_replay) !=
            jfg::InputReplayError::none) {
      std::fputs("native boot setup failed: input replay\n", stderr);
      return 3;
    }
    state.input_replay_loaded = true;
    state.input_replay_event_count = state.input_replay.events().size();
  }
  if (const char *save_path = std::getenv("JFG_PHASE8_SAVE_PATH");
      save_path != nullptr && *save_path != '\0' &&
      (std::strlen(save_path) > 32767U ||
       !configure_flash_path(state, save_path))) {
    std::fputs("native boot setup failed: save path\n", stderr);
    return 3;
  }
  if (const char *controller_pak_path =
          std::getenv("JFG_PHASE8_CONTROLLER_PAK_PATH");
      controller_pak_path != nullptr && *controller_pak_path != '\0' &&
      (std::strlen(controller_pak_path) > 32767U ||
       !configure_controller_pak_path(state, controller_pak_path))) {
    std::fputs("native boot setup failed: controller pak path\n", stderr);
    return 3;
  }
#endif
  if (!state.progress_path.empty()) {
    state.timing_trace.open(state.progress_path + ".timing.tsv",
                            std::ios::out | std::ios::trunc);
    if (!state.timing_trace) {
      std::fputs("native boot setup failed: timing trace path\n", stderr);
      return 3;
    }
    state.timing_trace
        << "kind\tvi_frame\tvi_retrace\tduration_us\tdetail_a\tdetail_b"
           "\tdetail_c\tdetail_d\n";
  }
  if (!state.input_record_path.empty()) {
    state.input_record.open(state.input_record_path, std::ios::trunc);
    if (!state.input_record) {
      std::fputs("native boot setup failed: input record open\n", stderr);
      return 3;
    }
#if defined(_WIN32)
    reload_controller_ports(state);
    state.pc_record_extended=state.pc_experimental;
#endif
    state.input_record << (state.pc_record_extended?jfg::kInputReplayHeaderV3:jfg::kInputReplayHeader) << '\n';
  }
  if (!state.retrace_hash_path.empty()) {
    state.retrace_hash_trace.open(state.retrace_hash_path,
                                  std::ios::out | std::ios::trunc);
    if (!state.retrace_hash_trace) {
      std::fputs("native boot setup failed: retrace hash open\n", stderr);
      return 3;
    }
    state.retrace_hash_trace
        << "{\"kind\":\"jfg-phase9-retrace-hash-header\","
           "\"schema\":1}\n";
    char *updates = nullptr;
    std::size_t updates_size = 0U;
    if (_dupenv_s(&updates, &updates_size, "JFG_PHASE9_UPDATE_HASHES") != 0) {
      std::fputs("native boot setup failed: update hash environment\n", stderr);
      return 3;
    }
    const bool update_hash_enabled = updates != nullptr && std::strcmp(updates, "1") == 0;
    const bool update_hash_invalid = updates != nullptr && *updates != '\0' && !update_hash_enabled;
    std::free(updates);
    if (update_hash_invalid) {
      std::fputs("native boot setup failed: update hash value\n", stderr);
      return 3;
    }
    if (update_hash_enabled) {
      state.update_hash_trace.open(state.retrace_hash_path + ".updates.jsonl",
                                   std::ios::out | std::ios::trunc);
      if (!state.update_hash_trace) {
        std::fputs("native boot setup failed: update hash open\n", stderr);
        return 3;
      }
      state.update_hash_trace
          << "{\"kind\":\"jfg-phase9-update-hash-header\",\"schema\":1}\n";
      char *focus = nullptr;
      std::size_t focus_size = 0U;
      if (_dupenv_s(&focus, &focus_size, "JFG_PHASE9_FOCUS_UPDATES") != 0) {
        std::fputs("native boot setup failed: focus environment\n", stderr);
        return 3;
      }
      if (focus != nullptr && *focus != '\0') {
        const std::string_view specification(focus);
        const auto colon = specification.find(':');
        std::uint64_t first = 0U, last = 0U;
        bool valid = colon != std::string_view::npos && colon != 0U &&
                     colon + 1U < specification.size();
        if (valid) {
          const auto first_number = std::from_chars(
              specification.data(), specification.data() + colon, first);
          const auto last_number = std::from_chars(
              specification.data() + colon + 1U,
              specification.data() + specification.size(), last);
          valid = first_number.ec == std::errc{} &&
                  first_number.ptr == specification.data() + colon &&
                  last_number.ec == std::errc{} &&
                  last_number.ptr == specification.data() + specification.size();
        }
        std::free(focus);
        if (!valid || first < 1U || last < first || last - first > 15U) {
          std::fputs("native boot setup failed: focus update range\n", stderr);
          return 3;
        }
        state.focus_update_start = first;
        state.focus_update_end = last;
      } else {
        std::free(focus);
      }
    }
  }
  char *update_word = nullptr;
  std::size_t update_word_size = 0U;
  if (_dupenv_s(&update_word, &update_word_size,
                "JFG_PHASE9_UPDATE_WORD") != 0) {
    std::fputs("native boot setup failed: update word environment\n", stderr);
    return 3;
  }
  if (update_word != nullptr && *update_word != '\0') {
    const std::string_view specification(update_word);
    std::uint32_t address = 0U;
    bool valid = specification.size() == 10U &&
                 specification.substr(0U, 2U) == "0x";
    if (valid) {
      const auto parsed = std::from_chars(specification.data() + 2U,
                                          specification.data() + 10U,
                                          address, 16);
      valid = parsed.ec == std::errc{} &&
              parsed.ptr == specification.data() + 10U;
    }
    std::free(update_word);
    if (!valid || address < 0x80000000U || address > 0x803FFFFCU ||
        (address & 3U) != 0U || !state.update_hash_trace.is_open()) {
      std::fputs("native boot setup failed: update word range\n", stderr);
      return 3;
    }
    state.update_word_address = address;
    const auto update_word_path = std::filesystem::path(state.retrace_hash_path).parent_path() /
        "update-word.tsv";
    state.update_word_trace.open(update_word_path, std::ios::out | std::ios::trunc);
    if (!state.update_word_trace) {
      std::fputs("native boot setup failed: update word trace\n", stderr);
      return 3;
    }
    state.update_word_trace <<
        "update\tcontroller_polls\tvi\tframe\taddress\tvalue\n";
  } else {
    std::free(update_word);
  }
  char *watch = nullptr;
  std::size_t watch_size = 0U;
  if (_dupenv_s(&watch, &watch_size, "JFG_PHASE9_WATCH_WORD") != 0) {
    std::fputs("native boot setup failed: watch environment\n", stderr);
    return 3;
  }
  if (watch != nullptr && *watch != '\0') {
    const std::string_view specification(watch);
    std::uint32_t address = 0U;
    bool valid = specification.size() == 10U &&
                 specification.substr(0U, 2U) == "0x";
    if (valid) {
      const auto parsed = std::from_chars(specification.data() + 2U,
                                          specification.data() + 10U,
                                          address, 16);
      valid = parsed.ec == std::errc{} &&
              parsed.ptr == specification.data() + 10U;
    }
    std::free(watch);
    if (!valid || address < 0x80000000U || address > 0x803FFFFCU ||
        (address & 3U) != 0U || state.focus_update_start == 0U ||
        !state.update_hash_trace.is_open()) {
      std::fputs("native boot setup failed: watch word range\n", stderr);
      return 3;
    }
    state.watch_word_address = address;
    const auto watch_path = std::filesystem::path(state.retrace_hash_path).parent_path() /
        "watch-word.tsv";
    state.watch_word_trace.open(watch_path, std::ios::out | std::ios::trunc);
    if (!state.watch_word_trace) {
      std::fputs("native boot setup failed: watch word trace\n", stderr);
      return 3;
    }
    state.watch_word_trace <<
        "update_candidate\tvi_retraces\tcontroller_polls\tdepth\t"
        "dispatch_target\treturn_address\tbefore\tafter\n";
  } else {
    std::free(watch);
  }
  char *entry = nullptr;
  std::size_t entry_size = 0U;
  if (_dupenv_s(&entry, &entry_size, "JFG_PHASE9_ENTRY_TARGET") != 0) {
    std::fputs("native boot setup failed: entry environment\n", stderr);
    return 3;
  }
  if (entry != nullptr && *entry != '\0') {
    const std::string_view specification(entry);
    std::uint32_t target = 0U;
    bool valid = specification.size() == 10U &&
                 specification.substr(0U, 2U) == "0x";
    if (valid) {
      const auto parsed = std::from_chars(specification.data() + 2U,
                                          specification.data() + 10U,
                                          target, 16);
      valid = parsed.ec == std::errc{} &&
              parsed.ptr == specification.data() + 10U;
    }
    std::free(entry);
    if (!valid || target == 0U || (target & 3U) != 0U ||
        state.focus_update_start == 0U || !state.update_hash_trace.is_open()) {
      std::fputs("native boot setup failed: entry target range\n", stderr);
      return 3;
    }
    state.entry_probe_target = target;
    const auto entry_path =
        std::filesystem::path(state.retrace_hash_path).parent_path() /
        "entry-args.tsv";
    state.entry_probe_trace.open(entry_path, std::ios::out | std::ios::trunc);
    if (!state.entry_probe_trace) {
      std::fputs("native boot setup failed: entry trace\n", stderr);
      return 3;
    }
    state.entry_probe_trace <<
        "update_candidate\tvi_retraces\tcontroller_polls\ttarget\t"
        "a0\ta1\ta2\ta3\n";
  } else {
    std::free(entry);
  }
  char *entry_fpu = nullptr;
  std::size_t entry_fpu_size = 0U;
  if (_dupenv_s(&entry_fpu, &entry_fpu_size, "JFG_PHASE9_ENTRY_FPU") != 0) {
    std::fputs("native boot setup failed: entry FPU environment\n", stderr);
    return 3;
  }
  const bool trace_entry_fpu = entry_fpu != nullptr &&
      std::string_view(entry_fpu) == "1";
  const bool invalid_entry_fpu = entry_fpu != nullptr &&
      *entry_fpu != '\0' && !trace_entry_fpu;
  std::free(entry_fpu);
  if (invalid_entry_fpu || (trace_entry_fpu && state.entry_probe_target == 0U)) {
    std::fputs("native boot setup failed: entry FPU requires entry probe\n", stderr);
    return 3;
  }
  if (trace_entry_fpu) {
    const auto fpu_path =
        std::filesystem::path(state.retrace_hash_path).parent_path() /
        "entry-fpu.tsv";
    state.entry_fpu_trace.open(fpu_path, std::ios::out | std::ios::trunc);
    if (!state.entry_fpu_trace) {
      std::fputs("native boot setup failed: entry FPU trace\n", stderr);
      return 3;
    }
    state.entry_fpu_trace <<
        "update_candidate\tvi_retraces\tcontroller_polls\ttarget";
    for (std::size_t index = 0U; index < 32U; ++index)
      state.entry_fpu_trace << "\tf" << index << "_lo\tf" << index << "_hi";
    state.entry_fpu_trace << '\n';
  }
  char *entry_memory = nullptr;
  std::size_t entry_memory_size = 0U;
  if (_dupenv_s(&entry_memory, &entry_memory_size,
                "JFG_PHASE9_ENTRY_MEMORY") != 0) {
    std::fputs("native boot setup failed: entry memory environment\n", stderr);
    return 3;
  }
  const bool trace_entry_memory = entry_memory != nullptr &&
      std::string_view(entry_memory) == "1";
  const bool invalid_entry_memory = entry_memory != nullptr &&
      *entry_memory != '\0' && !trace_entry_memory;
  std::free(entry_memory);
  if (invalid_entry_memory ||
      (trace_entry_memory && state.entry_probe_target == 0U)) {
    std::fputs("native boot setup failed: entry memory requires entry probe\n",
               stderr);
    return 3;
  }
  if (trace_entry_memory) {
    const auto memory_path =
        std::filesystem::path(state.retrace_hash_path).parent_path() /
        "entry-memory.tsv";
    state.entry_memory_trace.open(memory_path, std::ios::out | std::ios::trunc);
    if (!state.entry_memory_trace) {
      std::fputs("native boot setup failed: entry memory trace\n", stderr);
      return 3;
    }
    state.entry_memory_trace <<
        "update_candidate\tvi_retraces\tcontroller_polls\ttarget\t"
        "a1_256\ta2_256\ta3_256\n";
  }
  char *entry_gpr = nullptr;
  std::size_t entry_gpr_size = 0U;
  if (_dupenv_s(&entry_gpr, &entry_gpr_size, "JFG_PHASE9_ENTRY_GPR") != 0) {
    std::fputs("native boot setup failed: entry GPR environment\n", stderr);
    return 3;
  }
  const bool trace_entry_gpr = entry_gpr != nullptr &&
      std::string_view(entry_gpr) == "1";
  const bool invalid_entry_gpr = entry_gpr != nullptr &&
      *entry_gpr != '\0' && !trace_entry_gpr;
  std::free(entry_gpr);
  if (invalid_entry_gpr || (trace_entry_gpr && state.entry_probe_target == 0U)) {
    std::fputs("native boot setup failed: entry GPR requires entry probe\n", stderr);
    return 3;
  }
  if (trace_entry_gpr) {
    const auto gpr_path =
        std::filesystem::path(state.retrace_hash_path).parent_path() /
        "entry-gpr.tsv";
    state.entry_gpr_trace.open(gpr_path, std::ios::out | std::ios::trunc);
    if (!state.entry_gpr_trace) {
      std::fputs("native boot setup failed: entry GPR trace\n", stderr);
      return 3;
    }
    state.entry_gpr_trace <<
        "update_candidate\tvi_retraces\tcontroller_polls\ttarget";
    for (std::size_t index = 0U; index < 32U; ++index)
      state.entry_gpr_trace << "\tr" << index << "_lo\tr" << index << "_hi";
    state.entry_gpr_trace << '\n';
  }
  char *instruction_probe = nullptr;
  const auto point_addresses = [](const char* name) {
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0)
      return std::optional<std::vector<std::uint32_t>>{};
    const auto parsed = point_probe_addresses(raw == nullptr ? "" : raw);
    std::free(raw);
    return parsed;
  };
  const auto point_pcs = point_addresses("JFG_PHASE9_POINT_PCS");
  const auto point_words = point_addresses("JFG_PHASE9_POINT_WORDS");
  if (!point_pcs || !point_words ||
      (point_pcs->empty() && !point_words->empty()) ||
      (!point_pcs->empty() && (!state.guest_os_probe || state.focus_update_start == 0U ||
                              !state.update_hash_trace.is_open()))) {
    std::fputs("native boot setup failed: point probe requires original OS and focused updates\n", stderr);
    return 3;
  }
  state.point_probe_pcs = *point_pcs;
  state.point_probe_words = *point_words;
  char* device_events = nullptr;
  std::size_t device_events_size = 0U;
  if (_dupenv_s(&device_events, &device_events_size, "JFG_PHASE9_DEVICE_EVENTS") != 0) return 3;
  const bool trace_device_events = device_events != nullptr && std::string_view(device_events) == "1";
  const bool invalid_device_events = device_events != nullptr && !trace_device_events;
  std::free(device_events);
  if (invalid_device_events || (trace_device_events && point_pcs->empty())) {
    std::fputs("native boot setup failed: device events require focused original-OS point capture\n", stderr);
    return 3;
  }
  if (trace_device_events) {
    const auto device_event_path = std::filesystem::path(state.retrace_hash_path).parent_path() / "device-events.tsv";
    FILE* device_event_output = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&device_event_output, device_event_path.c_str(), L"wb") != 0 || device_event_output == nullptr) return 3;
#else
    device_event_output = std::fopen(device_event_path.c_str(), "wb");
    if (device_event_output == nullptr) return 3;
#endif
    state.device_event_stream.reset(device_event_output);
    const auto event_first = static_cast<std::uint32_t>((std::max)(std::uint64_t{1}, state.focus_update_start - 1U));
    const auto event_last = static_cast<std::uint32_t>(state.focus_update_end + 1U);
    if (!jfg_device_event_open(&state.device_event_probe, device_event_output, event_first, event_last, JFG_EVENT_NATIVE_PRE_INSTRUCTION)) return 3;
  }
  char* eret_transfers = nullptr;
  std::size_t eret_transfers_size = 0U;
  if (_dupenv_s(&eret_transfers, &eret_transfers_size, "JFG_PHASE9_ERET_TRANSFERS") != 0) return 3;
  const bool trace_eret_transfers = eret_transfers != nullptr && std::string_view(eret_transfers) == "1";
  const bool invalid_eret_transfers = eret_transfers != nullptr && !trace_eret_transfers;
  std::free(eret_transfers);
  if (invalid_eret_transfers || (trace_eret_transfers && !trace_device_events)) return 3;
  if (trace_eret_transfers) {
    state.eret_transfer_stream.open(std::filesystem::path(state.retrace_hash_path).parent_path() / "eret-transfers.tsv");
    if (!state.eret_transfer_stream || !state.eret_transfer_probe.open(state.eret_transfer_stream,
        state.device_event_probe.first, state.device_event_probe.last)) return 3;
  }
  char* instruction_effects = nullptr;
  std::size_t instruction_effects_size = 0U;
  if (_dupenv_s(&instruction_effects, &instruction_effects_size, "JFG_PHASE9_INSTRUCTION_EFFECT_UPDATE") != 0) return 3;
  if (instruction_effects != nullptr) {
    const std::string_view value(instruction_effects);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), state.instruction_effect_update);
    const bool valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
        state.guest_os_probe && trace_eret_transfers && trace_device_events &&
        state.instruction_effect_update >= state.device_event_probe.first &&
        state.instruction_effect_update <= state.device_event_probe.last;
    std::free(instruction_effects);
    if (!valid) return 3;
    state.instruction_effect_stream.open(std::filesystem::path(state.retrace_hash_path).parent_path() /
                                        "instruction-effects.bin", std::ios::binary);
    if (!state.instruction_effect_stream || !state.instruction_effect_trace.open(
          state.instruction_effect_stream, state.instruction_effect_update)) return 3;
  }
  if (!point_pcs->empty()) {
    state.point_probe_trace.open(std::filesystem::path(state.retrace_hash_path).parent_path() /
                                "point-probe.tsv", std::ios::out | std::ios::trunc);
    if (!state.point_probe_trace) return 3;
    state.point_probe_trace << "update_candidate\tvi_retraces\tcontroller_polls\tpc\topcode";
    for (unsigned index = 0; index < 32; ++index)
      state.point_probe_trace << "\tr" << index << "_lo\tr" << index << "_hi";
    for (const auto address : state.point_probe_words)
      state.point_probe_trace << "\tm" << std::hex << std::setw(8) << std::setfill('0') << address;
    state.point_probe_trace << std::dec << std::setfill(' ') << '\n';
  }
  std::size_t instruction_probe_size = 0U;
  if (_dupenv_s(&instruction_probe, &instruction_probe_size,
                "JFG_PHASE9_INSTRUCTION_PROBE") != 0) {
    std::fputs("native boot setup failed: instruction probe environment\n", stderr);
    return 3;
  }
  const bool trace_instruction = instruction_probe != nullptr &&
      std::string_view(instruction_probe) == "1";
  const bool invalid_instruction = instruction_probe != nullptr &&
      *instruction_probe != '\0' && !trace_instruction;
  std::free(instruction_probe);
  if (invalid_instruction ||
      (trace_instruction && (state.focus_update_start == 0U ||
                             !state.update_hash_trace.is_open()))) {
    std::fputs("native boot setup failed: instruction probe requires focused updates\n",
               stderr);
    return 3;
  }
  if (trace_instruction) {
    const auto instruction_path =
        std::filesystem::path(state.retrace_hash_path).parent_path() /
        "instruction-probe.tsv";
    state.instruction_probe_trace.open(instruction_path,
                                       std::ios::out | std::ios::trunc);
    if (!state.instruction_probe_trace) {
      std::fputs("native boot setup failed: instruction probe trace\n", stderr);
      return 3;
    }
    state.instruction_probe_trace <<
        "update_candidate\tvi_retraces\tcontroller_polls\tpc\t"
        "r4\tr5\tr6\tr7\tr10\tr15\tr19\tr20\tr21\tr23\tr25\t"
        "f4_lo\tf22_lo\tf23_odd_lo\thost_fe_round\n";
  }
#else
  if (play_mode)
    return 3;
#endif
  state.runtime = &runtime;
  state.rdram = rdram.data();
  state.rom = rom.data();
  state.rom_size = rom.size();
  if (state.navigation_mod.observing() && !state.navigation_mod.npc_rewards.load_rom(rom))
    std::fputs("navigation mod: NPC reward catalog unavailable; rewards remain unknown\n", stderr);
#if defined(JFG_PHASE8_LIVE_RUNTIME)
  char *poll_path = nullptr;
  std::size_t poll_path_size = 0U;
  if (_dupenv_s(&poll_path, &poll_path_size,
                "JFG_PHASE95_NATIVE_POLL_TRACE") != 0) {
    std::fputs("native boot setup failed: poll trace environment\n", stderr);
    return 3;
  }
  if (poll_path != nullptr && *poll_path != '\0') {
    if (poll_path_size > 32768U) {
      std::free(poll_path);
      std::fputs("native boot setup failed: poll trace path\n", stderr);
      return 3;
    }
    state.phase95_poll_trace.open(poll_path, std::ios::out | std::ios::trunc);
    if (!state.phase95_poll_trace) {
      std::free(poll_path);
      std::fputs("native boot setup failed: poll trace open\n", stderr);
      return 3;
    }
    state.phase95_poll_trace
        << "poll\tvi_retrace\tvi_frame\tfront_mode\tlevel_word"
           "\trng_seed\tbuttons\tx\ty\n";
  }
  std::free(poll_path);
  char *poll_hashes = nullptr;
  std::size_t poll_hashes_size = 0U;
  if (_dupenv_s(&poll_hashes, &poll_hashes_size,
                "JFG_PHASE9_POLL_HASHES") != 0) {
    std::fputs("native boot setup failed: poll hash environment\n", stderr);
    return 3;
  }
  const bool poll_hash_enabled = poll_hashes != nullptr &&
      std::strcmp(poll_hashes, "1") == 0;
  const bool poll_hash_invalid = poll_hashes != nullptr &&
      *poll_hashes != '\0' && !poll_hash_enabled;
  std::free(poll_hashes);
  if (poll_hash_invalid || (poll_hash_enabled &&
      (!state.input_replay_by_poll || state.retrace_hash_path.empty()))) {
    std::fputs("native boot setup failed: poll hash requires replay-by-poll\n", stderr);
    return 3;
  }
  if (poll_hash_enabled) {
    state.poll_hash_trace.open(state.retrace_hash_path + ".polls.jsonl",
                               std::ios::out | std::ios::trunc);
    if (!state.poll_hash_trace) {
      std::fputs("native boot setup failed: poll hash open\n", stderr);
      return 3;
    }
    state.poll_hash_trace <<
        "{\"kind\":\"jfg-phase9-poll-hash-header\",\"schema\":1,"
        "\"boundary\":\"pre-controller-input\"}\n";
  }
  char *event_range = nullptr;
  std::size_t event_range_size = 0U;
  if (_dupenv_s(&event_range, &event_range_size,
                "JFG_PHASE9_EVENT_TRACE_RANGE") != 0) {
    std::fputs("native boot setup failed: event trace environment\n", stderr);
    return 3;
  }
  if (event_range != nullptr && *event_range != '\0') {
    const std::string_view specification(event_range);
    bool valid = poll_hash_enabled && state.update_hash_trace.is_open() &&
                 specification.size() <= 64U;
    std::size_t cursor = 0U;
    while (valid && cursor < specification.size()) {
      if (state.event_range_count == state.event_ranges.size()) {
        valid = false;
        break;
      }
      const auto comma = specification.find(',', cursor);
      const auto end = comma == std::string_view::npos
                           ? specification.size() : comma;
      const auto colon = specification.find(':', cursor);
      std::uint64_t first = 0U, last = 0U;
      valid = colon != std::string_view::npos && colon > cursor &&
              colon + 1U < end && colon < end;
      if (valid) {
        const auto left = std::from_chars(
            specification.data() + cursor, specification.data() + colon,
            first);
        const auto right = std::from_chars(
            specification.data() + colon + 1U, specification.data() + end,
            last);
        valid = left.ec == std::errc{} &&
                left.ptr == specification.data() + colon &&
                right.ec == std::errc{} &&
                right.ptr == specification.data() + end &&
                last >= first && last - first <= 31U && last <= 1000000U &&
                (state.event_range_count == 0U ||
                 first > state.event_ranges[state.event_range_count - 1U].second);
      }
      if (valid)
        state.event_ranges[state.event_range_count++] = {first, last};
      if (comma == std::string_view::npos)
        break;
      cursor = comma + 1U;
      valid = valid && cursor < specification.size();
    }
    if (!valid || state.event_range_count == 0U) {
      std::free(event_range);
      std::fputs("native boot setup failed: event trace range\n", stderr);
      return 3;
    }
    state.event_trace.open(state.retrace_hash_path + ".events.tsv",
                           std::ios::out | std::ios::trunc);
    if (!state.event_trace) {
      std::free(event_range);
      std::fputs("native boot setup failed: event trace open\n", stderr);
      return 3;
    }
    state.event_trace << "sequence\tevent\tpoll\tcompleted_updates\tframe"
                         "\tvi\tstart_calls\tget_calls\tconnected\tbuttons"
                         "\tstick_x\tstick_y\n";
  }
  std::free(event_range);
  char *controller_return = nullptr;
  std::size_t controller_return_size = 0U;
  if (_dupenv_s(&controller_return, &controller_return_size,
                "JFG_PHASE9_CONTROLLER_RETURN") != 0) {
    std::fputs("native boot setup failed: controller return environment\n", stderr);
    return 3;
  }
  if (controller_return != nullptr && *controller_return != '\0') {
    const bool valid = std::strcmp(controller_return, "1") == 0 &&
                       state.event_range_count != 0U;
    std::free(controller_return);
    if (!valid) {
      std::fputs("native boot setup failed: controller return range\n", stderr);
      return 3;
    }
    state.controller_return_trace.open(
        state.retrace_hash_path + ".controller-return.tsv",
        std::ios::out | std::ios::trunc);
    if (!state.controller_return_trace) {
      std::fputs("native boot setup failed: controller return trace\n", stderr);
      return 3;
    }
    state.controller_return_trace <<
        "sequence\tpoll\tcompleted_updates\tvi\tframe\taddress\tdata\n";
  } else {
    std::free(controller_return);
  }
  std::uint32_t graphics_shadow_cursor =
      static_cast<std::uint32_t>(kRdramSize);
  for (std::uint32_t section = 0U;
       section < jfg_generated_section_count(); ++section) {
    JfgGeneratedSectionMetadata metadata{};
    if (jfg_generated_section_metadata(section, &metadata) == 0) {
      std::fputs("native boot setup failed: graphics overlay index\n",
                 stderr);
      return 3;
    }
    if (metadata.is_overlay == 0U)
      continue;
    const std::uint64_t extent = std::uint64_t{metadata.text_size} +
                                 metadata.data_size + metadata.bss_size;
    graphics_shadow_cursor = (graphics_shadow_cursor + 15U) & ~15U;
    if (extent == 0U || extent > UINT32_MAX - graphics_shadow_cursor ||
        graphics_shadow_cursor + extent > jfg::kRt64RequiredRdramBytes) {
      std::fputs("native boot setup failed: graphics overlay index\n",
                 stderr);
      return 3;
    }
    state.graphics_overlay_shadows.push_back(
        {section, metadata.linked_vram, static_cast<std::uint32_t>(extent),
         graphics_shadow_cursor});
    graphics_shadow_cursor += static_cast<std::uint32_t>(extent);
  }
#endif
  for (std::uint32_t source = 0U;
       source < jfg_generated_section_count(); ++source) {
    const JfgGeneratedR32Descriptor *descriptors = nullptr;
    std::size_t count = 0U;
    if (jfg_generated_relocation_descriptors(source, &descriptors, &count) ==
            0 ||
        (count != 0U && descriptors == nullptr)) {
      std::fputs("native boot setup failed: relocation target index\n",
                 stderr);
      return 3;
    }
    for (std::size_t index = 0U; index < count; ++index) {
      JfgGeneratedSectionMetadata target_metadata{};
      if (jfg_generated_section_metadata(descriptors[index].target_section,
                                         &target_metadata) == 0 ||
          descriptors[index].target_offset >
              UINT32_MAX - target_metadata.linked_vram) {
        std::fputs("native boot setup failed: relocation target index\n",
                   stderr);
        return 3;
      }
      state.generated_relocation_targets.insert(
          target_metadata.linked_vram + descriptors[index].target_offset);
    }
  }
#if defined(JFG_LIVE_RUNTIME)
  char hidden_validation[2]{};
  const bool diagnostic_hidden_window =
      GetEnvironmentVariableA("JFG_DIAGNOSTIC_HIDDEN_WINDOW",hidden_validation,2U)==1U &&
      hidden_validation[0]=='1';
  LiveRt64Window live_window(state, play_mode && !diagnostic_hidden_window);
  if (!initialize_live_renderer(state, live_window)) {
    std::fputs("native boot setup failed: RT64 renderer\n", stderr);
    return 3;
  }
#endif
  jfg::support_event("native=renderer-ready");
  state.mmio_trace = &backing.mmio_trace();
  state.io_revision = &state.mmio_trace->execution_io_revision;
  state.io_guest_count = &state.mmio_trace->guest_count;
  if (state.guest_os_probe) {
    // Read-only, ROM-pinned corrected-Mupen IPL hardware observation. This
    // initializes platform registers only, not game actors, RNG or save RAM.
    // The reset interpreter contributes its actual executed instructions.
    // Startup phase and progressive scanlines passed the independent phase
    // microtest; no game-parity acceptance follows from this profile.
    state.cpu_count = 14421016U + 2U * handoff.instructions;
    state.mmio_trace->guest_execution = true;
    state.mmio_trace->device_rdram = {state.rdram, kRdramSize};
    state.mmio_trace->device_rom = {state.rom, state.rom_size};
    // IPL3 programs cartridge-domain timing from the ROM header before boot.
    // Recover that platform state because this private entry skips IPL3.
    if (state.rom_size < 4U) return 3;
    const auto cartridge_timing = (std::uint32_t(state.rom[0]) << 24U) |
        (std::uint32_t(state.rom[1]) << 16U) |
        (std::uint32_t(state.rom[2]) << 8U) | std::uint32_t(state.rom[3]);
    const std::array<std::uint32_t, 4> pi_boot_timing{
        cartridge_timing & 255U, (cartridge_timing >> 8U) & 255U,
        (cartridge_timing >> 16U) & 15U, (cartridge_timing >> 20U) & 3U};
    for (std::size_t index = 0U; index < pi_boot_timing.size(); ++index) {
      if (!state.mmio_trace->pi_dma.write(0x14U + static_cast<std::uint32_t>(index) * 4U,
              pi_boot_timing[index], state.cpu_count, state.mmio_trace->device_rdram,
              state.mmio_trace->device_rom)) return 3;
    }

    state.mmio_trace->flash_bus.bind(state.flashram);
    state.mmio_trace->guest_count = state.cpu_count;
    state.mmio_trace->mi_pending = 8U;
    state.mmio_trace->vi_next = 5000U + ((state.cpu_count - 5000U) / 500000U + 1U) * 500000U;
    char* guest_trace_path = nullptr;
    std::size_t guest_trace_path_size = 0U;
    if (_dupenv_s(&guest_trace_path, &guest_trace_path_size, "JFG_PHASE9_GUEST_OS_CLOCK_TRACE") != 0)
      return 3;
    if (guest_trace_path != nullptr && *guest_trace_path != '\0') {
      state.guest_clock_trace.open(guest_trace_path, std::ios::binary | std::ios::trunc);
      std::free(guest_trace_path);
      if (!state.guest_clock_trace) return 3;
      state.guest_clock_trace << "boot\t" << state.cpu_count << '\t' << handoff.instructions << '\n';
      state.guest_clock_trace << "pi-boot\t" << cartridge_timing;
      for (auto value : pi_boot_timing) state.guest_clock_trace << '\t' << value;
      state.guest_clock_trace << '\n';
    } else {
      std::free(guest_trace_path);
      if (!original_timing_mode) {
        std::fputs("native boot setup failed: guest OS diagnostic trace required\n", stderr);
        return 3;
      }
    }

  }
  if (state.guest_leaf_probe) {
    // Explicitly observed reference boot state, not a hardware-reset claim.
    if (!state.mmio_trace->mi_mask.initialize(0U)) return 3;
    state.mmio_trace->guest_mi_mask = true;
    // The HLE task engine is initially stopped, with no outstanding task.
    // This explicit owned-device state is not a general hardware reset model.
    if (!state.mmio_trace->sp_status.initialize(1U, false)) return 3;
    state.mmio_trace->guest_sp_status = true;
    state.mmio_trace->sp_rdram = {state.rdram, kRdramSize};
    state.mmio_trace->guest_pif_boot = true;
  }
  if (!backing.arm_mmio_monitor(state.journal)) {
    std::fputs("native boot setup failed: MMIO monitor\n", stderr);
    return 3;
  }
  state.scheduler = std::make_unique<ThreadScheduler>(hle::GuestMemory(
      {rdram.data(), rdram.size()},
      hle::GuestMemory::Layout::native_word_big_endian));
  if (state.guest_leaf_probe && !state.scheduler->set_baton_hook(
      [&state](int thread, bool entering) {
        const auto index = static_cast<std::size_t>(thread);
        if (thread < 0 || index >= state.saved_rcp_masks.size())
          fail_closed_dispatch(state, "guest-leaf", "thread-mask-owner", 0U);
        // The independent thread fixture qualifies the ordinary unrestricted
        // OS global policy. Restricted global policies need their own saved
        // versus effective-mask qualification; do not silently approximate.
        std::uint32_t global = 0U;
        if (!state.scheduler->memory().read_u32(0x800a9a6cU, global) ||
            global != 0x003fff01U)
          fail_closed_dispatch(state, "guest-leaf", "thread-mask-global-policy", global);
        if (entering) {
          if (!state.mmio_trace->mi_mask.initialize(state.saved_rcp_masks[index]))
            fail_closed_dispatch(state, "guest-leaf", "thread-mask-restore", 0U);
        } else {
          state.saved_rcp_masks[index] = state.mmio_trace->mi_mask.read();
        }
      })) return 3;
  if (jfg_minimal_runtime_initialize() == 0 ||
      !runtime.install_table(std::make_unique<Table>()).ok() ||
      jfg_minimal_runtime_bind_dispatch(dispatch, &state) == 0) {
    std::fputs("native boot setup failed: generated runtime\n", stderr);
    return 3;
  }
  // Diagnostic selection and CPU ownership are immutable after setup.
  // Avoid inspecting stream/vector internals at every guest instruction.
  state.execution_fast_profile_ready = state.guest_os_probe &&
      state.mmio_trace != nullptr && state.point_probe_pcs.empty() &&
      !state.device_event_stream && !state.eret_transfer_stream.is_open();
  jfg::support_event("native=running");
  DispatchBinding binding{state, true};
  if (state.guest_leaf_probe) {
    if (jfg_minimal_runtime_bind_cpu(native_cpu_operation, &state) == 0) {
      std::fputs("native boot setup failed: CPU operation owner\n", stderr);
      return 3;
    }
    binding.cpu_bound = true;
  }
  const auto function =
      runtime.lookup_function_by_guest_address(handoff.transfer_target);
  if (!function.ok())
    ledger(state, "generated", "reset-transfer-unresolved",
           handoff.transfer_target);
  else {
    recomp_context context{};
    constexpr std::array<gpr recomp_context::*, 32> kGprMembers = {
        &recomp_context::r0,  &recomp_context::r1,  &recomp_context::r2,
        &recomp_context::r3,  &recomp_context::r4,  &recomp_context::r5,
        &recomp_context::r6,  &recomp_context::r7,  &recomp_context::r8,
        &recomp_context::r9,  &recomp_context::r10, &recomp_context::r11,
        &recomp_context::r12, &recomp_context::r13, &recomp_context::r14,
        &recomp_context::r15, &recomp_context::r16, &recomp_context::r17,
        &recomp_context::r18, &recomp_context::r19, &recomp_context::r20,
        &recomp_context::r21, &recomp_context::r22, &recomp_context::r23,
        &recomp_context::r24, &recomp_context::r25, &recomp_context::r26,
        &recomp_context::r27, &recomp_context::r28, &recomp_context::r29,
        &recomp_context::r30, &recomp_context::r31,
    };
    for (std::size_t index = 0U; index < handoff.gpr.size(); ++index)
      context.*kGprMembers[index] =
          static_cast<gpr>(sign_extend_mips32(handoff.gpr[index]));
    context.f_odd = &context.f0.u32h;
    context.status_reg = 0U;
    context.mips3_float_mode = 0U;
    if (state.guest_leaf_probe)
      cop0_status_write(&context, 0x34000000U); // observed reference cold boot
    if (state.guest_os_probe) {
      state.guest_transport = std::make_unique<GuestThreadTransport<recomp_context>>(
          [&state](std::uint32_t entry, recomp_context& cpu) {
            const auto body = state.runtime->lookup_function_by_guest_address(entry);
            if (!body.ok()) fail_closed_dispatch(state, "guest-os", "thread-entry-unavailable", entry);
            invoke_generated(body.function, state.rdram, &cpu, state, entry);
          }, [](recomp_context& cpu) {
            cpu.f_odd = cpu.mips3_float_mode ? &cpu.f1.u32l : &cpu.f0.u32h;
            set_cop1_cs(cpu.fcr31);
          }, [&state](const GuestThreadTransport<recomp_context>::EretBoundary& edge,
                      const recomp_context& cpu) {
            if (!state.eret_transfer_stream.is_open() && !state.instruction_effect_stream.is_open()) return;
            if (!state.guest_eret_boundary || state.guest_epc != edge.target_pc)
              fail_closed_dispatch(state, "eret-transfer-probe", "uncommitted-return", state.guest_last_pc);
            emit_instruction_effect(state, cpu, 2U, state.instruction_effect_section,
                state.guest_last_pc, state.guest_last_word, edge.target_pc, edge.to_thread);
            if (!state.eret_transfer_stream.is_open()) return;
            const EretTransferRow row{
              static_cast<std::uint32_t>(state.completed_game_updates + 1U),
              state.guest_last_pc, state.guest_last_word, edge.from_thread, edge.to_thread,
              edge.target_pc, cpu.status_reg, static_cast<std::uint32_t>(state.cpu_count),
              {0, cpu.r1, cpu.r2, cpu.r3, cpu.r4, cpu.r5, cpu.r6, cpu.r7,
               cpu.r8, cpu.r9, cpu.r10, cpu.r11, cpu.r12, cpu.r13, cpu.r14, cpu.r15,
               cpu.r16, cpu.r17, cpu.r18, cpu.r19, cpu.r20, cpu.r21, cpu.r22, cpu.r23,
               cpu.r24, cpu.r25, cpu.r26, cpu.r27, cpu.r28, cpu.r29, cpu.r30, cpu.r31}};
            if (!state.eret_transfer_probe.emit(row))
              fail_closed_dispatch(state, "eret-transfer-probe", "output-or-boundary", state.guest_last_pc);
          });
      try {
        state.guest_transport->run(handoff.transfer_target, context,
            state.play_mode ? UINT64_MAX : 1000000U);
      } catch (const std::exception& error) {
        std::fprintf(stderr, "guest OS transport failure: %s\n", error.what());
        fail_closed_dispatch(state, "guest-os", "transport-contract", state.guest_last_pc);
      }
    } else {
      invoke_generated(function.function, rdram.data(), &context, state,
                       handoff.transfer_target);
    }
  }
  if (!state.ledgered)
    ledger(state, "generated", "returned", handoff.transfer_target);
  return 4;
}

bool reset_transfer_target(const std::vector<std::uint8_t> &rom,
                           std::uint32_t &target) {
  std::vector<std::uint8_t> bytes(kRdramSize);
  if (!load_static_sections(rom, bytes))
    return false;
  const auto handoff = interpret_reset_handoff(
      bytes, 0x80000400U, kRdramSize / sizeof(std::uint32_t) + 4096U);
  if (!handoff.ok())
    return false;
  target = handoff.transfer_target;
  return true;
}

#if defined(_WIN32)
bool extract_canonical_ledger(const std::string &output,
                              std::string &ledger_line) {
  static const std::regex pattern(
      R"(\{"kind":"jfg-phase6-native-first-trap","category":"[a-z-]+","guest_target":"0x[0-9a-f]{8}","operation":"[A-Za-z_][A-Za-z0-9_-]*","disposition":"fail-closed-trap","count":1\}\r?\n)");
  std::sregex_iterator found(output.begin(), output.end(), pattern);
  if (found == std::sregex_iterator{} ||
      std::next(found) != std::sregex_iterator{}) {
    return false;
  }
  const std::smatch &match = *found;
  if (match.position() != 0U) {
    return false;
  }
  const std::string trailing = match.suffix().str();
  if (trailing.size() > 4096U ||
      trailing.find("jfg-phase6-native-first-trap") != std::string::npos) {
    return false;
  }
  if (!trailing.empty()) {
    return false;
  }
  ledger_line = match.str();
  if (ledger_line.size() >= 2U && ledger_line[ledger_line.size() - 2U] == '\r')
    ledger_line.erase(ledger_line.size() - 2U, 1U);
  return true;
}

bool extract_canonical_success(const std::string &output,
                               std::string &success_line) {
  static const std::regex pattern(
      R"jfg(\{"kind":"jfg-phase6-native-boot","status":"stable-vi","build_identity":"[A-Za-z0-9_.+-]+","sanitizer":"(?:none|address)","vi_retraces":3,"vi_frames":[1-9][0-9]*,"vi_interrupts":3,"vi_messages_delivered":3,"vi_queue":"0x[89a-f][0-9a-f]{7}","threads_created":[1-9][0-9]*,"rdram_bytes":4194304,"mmio_accesses":[1-9][0-9]*,"unsupported_accesses":0,"mmio_trace":"[rw]:0x[a-f0-9]{8}(?:,[rw]:0x[a-f0-9]{8})*","journal_entries":[1-9][0-9]*,"state_hash":"[0-9a-f]{64}","journal_hash":"[0-9a-f]{64}"\}\r?\n)jfg");
  std::smatch match;
  if (!std::regex_match(output, match, pattern))
    return false;
  success_line = match.str();
  if (success_line.size() >= 2U &&
      success_line[success_line.size() - 2U] == '\r')
    success_line.erase(success_line.size() - 2U, 1U);
  return true;
}

bool extract_canonical_probe(const std::string &output,
                             std::string &probe_line) {
  static const std::regex actor_trace_extension(
      R"jfg(\{"kind":"jfg-phase9-actor-trace","actors":\[.*\]\}\r?\n)jfg");
  static const std::regex king_bear_extension(
      R"jfg(,"hints_state":"0x[0-9a-f]{8}","hints_state_words":"(?:[0-9a-f]{8},){11}[0-9a-f]{8}","king_bear_control_calls":[0-9]+,"king_bear_callback":"0x[0-9a-f]{8}","king_bear_callback_section":[0-9]+,"king_bear_callback_offset":"0x[0-9a-f]{8}","king_bear_actor":"0x[0-9a-f]{8}","king_bear_yaw":"0x[0-9a-f]{4}","king_bear_position_bits":"[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}","king_bear_state":"0x[0-9a-f]{8}","king_bear_state_words":"[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}","hinttext_start_calls":[0-9]+,"hinttext_stop_calls":[0-9]+,"hinttext_active_calls":[0-9]+,"hinttext_active_zero_returns":[0-9]+,"hinttext_active_nonzero_returns":[0-9]+,"hinttext_active_last_return":[0-9]+,"hinttext_update_calls":[0-9]+,"hinttext_advance_calls":[0-9]+,"hinttext_accept_calls":[0-9]+,"hinttext_section_base":"0x[0-9a-f]{8}","hinttext_state_words":"(?:[0-9a-f]{8},){15}[0-9a-f]{8}","runlink_module7_words":"[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}")jfg");
  static const std::regex pattern(
      R"jfg(\{"kind":"jfg-phase8-native-probe","status":"retrace-target","retrace_target":(?:[4-9]|[1-9][0-9]+),"vi_retraces":(?:[4-9]|[1-9][0-9]+),"vi_frames":[1-9][0-9]*,"vi_interrupts":(?:[4-9]|[1-9][0-9]+),"vi_messages_delivered":(?:[4-9]|[1-9][0-9]+),"mmio_accesses":[1-9][0-9]*,"unsupported_accesses":[0-9]+,"dispatches":[0-9]+,"pi_dma":[0-9]+,"overlay_publications":[0-9]+,"overlay_sequence":"[0-9,]*","phase9_route":\{"player_control_calls":[0-9]+,"weapon_update_calls":[0-9]+,"weapon_fire_held_updates":[0-9]+,"weapon_fire_pressed_updates":[0-9]+,"weapon_dummy_fire_calls":[0-9]+,"weapon_fire_calls":[0-9]+,"weapon_shot_calls":[0-9]+,"weapon_hit_calls":[0-9]+,"enemy_kill_calls":[0-9]+,"weapon_stat_shots":[0-9]+,"weapon_stat_hits":[0-9]+,"weapon_stat_kills":[0-9]+,"collision_calls":[0-9]+,"particle_update_calls":[0-9]+,"cutscene_active_calls":[0-9]+,"cutscene_camera_calls":[0-9]+,"level_change_calls":[0-9]+,"death_restart_calls":[0-9]+,"player_hit_check_calls":[0-9]+,"player_actor":"0x[0-9a-f]{8}","player_yaw":"0x[0-9a-f]{4}","player_position_bits":"[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}","hints_control_calls":[0-9]+,"hints_talk_calls":[0-9]+,"hints_actor":"0x[0-9a-f]{8}","hints_yaw":"0x[0-9a-f]{4}","hints_position_bits":"[0-9a-f]{8},[0-9a-f]{8},[0-9a-f]{8}"\},"controller_events":[0-9]+,"generated_calls":[0-9]+,"mapped_calls":[0-9]+,"queue_create_calls":[0-9]+,"queue_send_calls":[0-9]+,"queue_recv_calls":[0-9]+,"sp_task_load_calls":[0-9]+,"sp_task_start_calls":[0-9]+,"controller_read_start_calls":[0-9]+,"controller_get_data_calls":[0-9]+,"vi_swap_calls":[0-9]+,"graphics_tasks":[0-9]+,"graphics_commands":[0-9]+,"graphics_triangles":[0-9]+,"presented_frames":[0-9]+,"decoded_audio_tasks":[0-9]+,"audio_device_initialized":(?:true|false),"audio_device_started":(?:true|false),"audio_device_frequency":[0-9]+,"audio_device_buffers":[0-9]+,"audio_device_queued_bytes":[0-9]+,"audio_device_consumed_bytes":[0-9]+,"audio_device_consumed_ms":[0-9]+,"audio_device_underruns":[0-9]+,"audio_device_overruns":[0-9]+,"ai_submission_overflows":[0-9]+,"frame_captured":(?:true|false),"input_replay_events":[0-9]+,"controller_samples":[0-9]+,"non_neutral_controller_samples":[0-9]+,"controller_disconnects":[0-9]+,"controller_reconnects":[0-9]+,"game_pressed_observations":[0-9]+,"game_stick_observations":[0-9]+,"last_game_stick_x":-?[0-9]+,"last_game_stick_y":-?[0-9]+,"menu_stick_observations":[0-9]+,"last_menu_stick_x":-?[0-9]+,"last_menu_stick_y":-?[0-9]+,"front_mode":[0-9]+,"flash_path_configured":(?:true|false),"flash_image_loaded":(?:true|false),"flash_image_created":(?:true|false),"flash_persist_count":[0-9]+,"threads_stopped":[0-9]+,"threads_runnable":[0-9]+,"threads_blocked":[0-9]+,"threads_finished":[0-9]+,"hle_counts":"[A-Za-z0-9_=,]+","thread_summary":"[0-9a-f:,]+","controller_read_thread":-?[0-9]+,"si_queue":"0x[0-9a-f]{8}","si_queue_valid":[0-9]+,"si_receive_calls":[0-9]+,"last_si_receive_return":"0x[0-9a-f]{8}","last_si_receive_block":[0-9]+,"last_si_dispatch_trace":"[0-9a-f,]+","journal_events":[0-9]+,"journal_entries":[0-9]+,"journal_overflow":(?:true|false),"state_hash":"[0-9a-f]{64}","journal_hash":"[0-9a-f]{64}"\}\r?\n)jfg");
  const std::string without_actor_trace =
      std::regex_replace(output, actor_trace_extension, "");
  const std::string canonical_output =
      std::regex_replace(without_actor_trace, king_bear_extension, "");
  if (canonical_output == without_actor_trace)
    return false;
  std::smatch match;
  if (!std::regex_match(canonical_output, match, pattern))
    return false;
  probe_line = output;
  if (probe_line.size() >= 2U &&
      probe_line[probe_line.size() - 2U] == '\r')
    probe_line.erase(probe_line.size() - 2U, 1U);
  return true;
}

bool extract_original_os_probe(const std::string& output, unsigned target, std::string& line) {
  char* enabled = nullptr;
  std::size_t size = 0;
  const bool opted_in = _dupenv_s(&enabled, &size, "JFG_PHASE9_GUEST_OS_PROBE") == 0 &&
      enabled && std::strcmp(enabled, "1") == 0;
  std::free(enabled);
  char original_timing[2]{};
  const auto timing_override = GetEnvironmentVariableA("JFG_ORIGINAL_TIMING",original_timing,2U);
  const bool runtime_opted_in = timing_override == 1U ? original_timing[0]=='1' :
      JFG_ORIGINAL_TIMING_GENERATION == 1;
  if (!opted_in && !runtime_opted_in) return false;
  static const std::regex pattern(
      R"jfg(\{"kind":"jfg-phase9-original-os-probe","acceptance":false,"status":"retrace-target","retrace_target":([1-9][0-9]*),"vi_retraces":([1-9][0-9]*),"controller_samples":[0-9]+,"completed_updates":[0-9]+,"cpu_count":[1-9][0-9]*,"instructions":[1-9][0-9]*,"graphics_tasks":[0-9]+,"decoded_audio_tasks":[0-9]+,"unsupported_accesses":0,"runtime_snapshot_complete":false,"state_hash":"[0-9a-f]{64}"\}\r?\n)jfg");
  std::smatch match;
  if (!std::regex_match(output, match, pattern) || match[1].str() != std::to_string(target) ||
      match[2].str() != std::to_string(target)) return false;
  line = output;
  return true;
}

bool extract_canonical_poll_probe(const std::string &output,
                                  const unsigned poll_target,
                                  std::string &probe_line) {
  // Reuse the strict retrace-probe suffix validator. Only the stop reason and
  // early VI counters differ; normalize those after checking their exact
  // order, decimal shape, requested target, and sampled-poll total.
  static const std::regex prefix(
      R"jfg(^\{"kind":"jfg-phase8-native-probe","status":"poll-target","poll_target":([1-9][0-9]*),"vi_retraces":([0-9]+),"vi_frames":([0-9]+),"vi_interrupts":([0-9]+),"vi_messages_delivered":([0-9]+),)jfg");
  static const std::regex samples(R"jfg(,"controller_samples":([0-9]+),)jfg");
  std::smatch prefix_match, sample_match;
  if (!std::regex_search(output, prefix_match, prefix) ||
      prefix_match[1].str() != std::to_string(poll_target) ||
      !std::regex_search(output, sample_match, samples) ||
      sample_match[1].str() != std::to_string(poll_target))
    return false;
  const std::string normalized =
      "{\"kind\":\"jfg-phase8-native-probe\",\"status\":\"retrace-target\","
      "\"retrace_target\":4,\"vi_retraces\":4,\"vi_frames\":4,"
      "\"vi_interrupts\":4,\"vi_messages_delivered\":4," +
      output.substr(prefix_match.length());
  std::string ignored;
  if (!extract_canonical_probe(normalized, ignored))
    return false;
  probe_line = output;
  if (probe_line.size() >= 2U &&
      probe_line[probe_line.size() - 2U] == '\r')
    probe_line.erase(probe_line.size() - 2U, 1U);
  return true;
}

bool extract_canonical_play_exit(const std::string &output,
                                 std::string &play_line) {
  static const std::regex pattern(
      R"jfg(\{"kind":"jfg-phase8-play","status":"window-closed","vi_retraces":[0-9]+,"graphics_tasks":[0-9]+,"presented_frames":[0-9]+,"decoded_audio_tasks":[0-9]+,"audio_device_initialized":(?:true|false),"audio_device_started":(?:true|false),"audio_device_frequency":[0-9]+,"audio_device_buffers":[0-9]+,"audio_device_consumed_bytes":[0-9]+,"audio_device_consumed_ms":[0-9]+,"audio_device_underruns":[0-9]+,"audio_device_overruns":[0-9]+,"frame_captured":(?:true|false),"controller_samples":[0-9]+,"non_neutral_controller_samples":[0-9]+,"non_neutral_controller_writes":[0-9]+,"controller_disconnects":[0-9]+,"controller_reconnects":[0-9]+,"game_pressed_observations":[0-9]+,"last_game_pressed_buttons":"0x[0-9a-f]{4}","game_stick_observations":[0-9]+,"last_game_stick_x":-?[0-9]+,"last_game_stick_y":-?[0-9]+,"menu_stick_observations":[0-9]+,"last_menu_stick_x":-?[0-9]+,"last_menu_stick_y":-?[0-9]+,"front_mode":[0-9]+,"unsupported_accesses":0,"journal_overflow":(?:true|false)\}\r?\n)jfg");
  std::smatch match;
  if (!std::regex_match(output, match, pattern))
    return false;
  play_line = match.str();
  if (play_line.size() >= 2U && play_line[play_line.size() - 2U] == '\r')
    play_line.erase(play_line.size() - 2U, 1U);
  return true;
}

std::wstring quote_argument(std::wstring_view value) {
  std::wstring quoted = L"\"";
  std::size_t slash_count = 0U;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++slash_count;
      continue;
    }
    if (character == L'\"') {
      quoted.append(slash_count * 2U + 1U, L'\\');
      quoted += L'\"';
    } else {
      quoted.append(slash_count, L'\\');
      quoted += character;
    }
    slash_count = 0U;
  }
  quoted.append(slash_count * 2U, L'\\');
  quoted += L"\"";
  return quoted;
}

bool read_file(HANDLE file, std::string &output) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 65536)
    return false;
  output.resize(static_cast<std::size_t>(size.QuadPart));
  SetFilePointer(file, 0, nullptr, FILE_BEGIN);
  DWORD read = 0U;
  return output.empty() ||
         (ReadFile(file, output.data(), static_cast<DWORD>(output.size()),
                   &read, nullptr) != 0 &&
          read == output.size());
}

bool wide_from_utf8(const char *input, std::wstring &output) {
  if (input == nullptr) {
    return false;
  }
  constexpr int kMaximumWideCharacters = 32768;
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, nullptr, 0);
  if (size <= 1 || size > kMaximumWideCharacters) {
    return false;
  }
  output.resize(static_cast<std::size_t>(size));
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                          output.data(), size) != size) {
    return false;
  }
  output.pop_back();
  return true;
}

bool parse_token(std::string_view text, std::array<std::uint8_t, 16U> &token) {
  if (text.size() != token.size() * 2U)
    return false;
  for (std::size_t index = 0; index < token.size(); ++index) {
    const auto digit = [](char value) -> int {
      if (value >= '0' && value <= '9')
        return value - '0';
      if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
      return -1;
    };
    const int high = digit(text[index * 2U]);
    const int low = digit(text[index * 2U + 1U]);
    if (high < 0 || low < 0)
      return false;
    token[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

bool validate_child_capability(const char *token_text,
                               const char *handle_text) {
  std::array<std::uint8_t, 16U> token{};
  if (token_text == nullptr || handle_text == nullptr ||
      !parse_token(token_text, token))
    return false;
  char *end = nullptr;
  const auto raw = std::strtoull(handle_text, &end, 16);
  if (*handle_text == '\0' || *end != '\0' || raw == 0U)
    return false;
  HANDLE marker = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
  DWORD flags = 0U;
  if (!GetHandleInformation(marker, &flags) ||
      (flags & HANDLE_FLAG_INHERIT) == 0U)
    return false;
  void *view = MapViewOfFile(marker, FILE_MAP_READ, 0, 0, token.size());
  if (view == nullptr)
    return false;
  const bool valid = std::memcmp(view, token.data(), token.size()) == 0;
  UnmapViewOfFile(view);
  return valid;
}

std::wstring token_text(const std::array<std::uint8_t, 16U> &token) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result(token.size() * 2U, L'0');
  for (std::size_t index = 0; index < token.size(); ++index) {
    result[index * 2U] = digits[token[index] >> 4U];
    result[index * 2U + 1U] = digits[token[index] & 15U];
  }
  return result;
}

bool module_filename(std::wstring &output) {
  std::vector<wchar_t> buffer(260U);
  for (;;) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (size == 0U || size >= buffer.size() - 1U || size > 32767U) {
      if (size == 0U || buffer.size() >= 32768U) {
        return false;
      }
      buffer.resize(buffer.size() * 2U);
      continue;
    }
    output.assign(buffer.data(), size);
    return true;
  }
}

int run_parent(const char *executable_path, const char *rom_path,
               unsigned timeout_ms, const unsigned retrace_target,
               const unsigned poll_target, const bool play_mode) {
  (void)executable_path;
  std::vector<std::uint8_t> validated_rom;
  if (!read_supported_rom(rom_path, validated_rom))
    return 2;
  std::uint32_t entry_target = 0U;
  if (!reset_transfer_target(validated_rom, entry_target))
    return 3;
  wchar_t temp_path[MAX_PATH + 1U]{};
  wchar_t stdout_path[MAX_PATH + 1U]{};
  if (GetTempPathW(MAX_PATH, temp_path) == 0U ||
      GetTempFileNameW(temp_path, L"jfg", 0U, stdout_path) == 0U)
    return 3;
  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;
  HANDLE output =
      CreateFileW(stdout_path, GENERIC_READ | GENERIC_WRITE, 0, &inheritable,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    DeleteFileW(stdout_path);
    return 3;
  }
  if (SetHandleInformation(output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ==
      0) {
    CloseHandle(output);
    DeleteFileW(stdout_path);
    return 3;
  }
  int result = 3;
  std::wstring executable;
  std::wstring rom;
  if (!module_filename(executable) || !wide_from_utf8(rom_path, rom)) {
    CloseHandle(output);
    DeleteFileW(stdout_path);
    return result;
  }
  std::wstring command = quote_argument(executable);
  command += L" --_jfg-phase6-child --rom ";
  command += quote_argument(rom);
  if (play_mode) {
    command += L" --play";
  } else if (poll_target != 0U) {
    command += L" --probe-polls ";
    command += std::to_wstring(poll_target);
  } else {
    command += L" --probe-retraces ";
    command += std::to_wstring(retrace_target);
  }
  HANDLE nul =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (nul == INVALID_HANDLE_VALUE) {
    CloseHandle(output);
    DeleteFileW(stdout_path);
    return result;
  }
  if (SetHandleInformation(nul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ==
      0) {
    CloseHandle(nul);
    CloseHandle(output);
    DeleteFileW(stdout_path);
    return result;
  }
  std::array<std::uint8_t, 16U> token{};
  HANDLE marker =
      CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0U,
                         static_cast<DWORD>(token.size()), nullptr);
  void *marker_view = marker == nullptr ? nullptr
                                        : MapViewOfFile(marker, FILE_MAP_WRITE,
                                                        0U, 0U, token.size());
  if (marker == nullptr || marker_view == nullptr ||
      SystemFunction036(token.data(), static_cast<ULONG>(token.size())) ==
          FALSE) {
    if (marker_view != nullptr)
      UnmapViewOfFile(marker_view);
    if (marker != nullptr)
      CloseHandle(marker);
    CloseHandle(nul);
    CloseHandle(output);
    DeleteFileW(stdout_path);
    return result;
  }
  if (SetHandleInformation(marker, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ==
      0) {
    CloseHandle(marker);
    CloseHandle(nul);
    CloseHandle(output);
    DeleteFileW(stdout_path);
    return result;
  }
  std::memcpy(marker_view, token.data(), token.size());
  UnmapViewOfFile(marker_view);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = nul;
  PROCESS_INFORMATION child{};
  command += L" --_jfg-phase6-token ";
  command += quote_argument(token_text(token));
  command += L" --_jfg-phase6-marker ";
  wchar_t marker_text[2U + sizeof(std::uintptr_t) * 2U + 1U]{};
  std::swprintf(marker_text, std::size(marker_text), L"%llx",
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(marker)));
  command += marker_text;
  if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                     &child) != 0) {
    const DWORD wait =
        WaitForSingleObject(child.hProcess, play_mode ? INFINITE : timeout_ms);
    DWORD exit_code = 0U;
    std::string child_output;
    if (wait == WAIT_TIMEOUT) {
      (void)TerminateProcess(child.hProcess, 4U);
      if (WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0) {
        State state{};
        ledger(state, "watchdog", "wall-clock", entry_target);
        result = 4;
      }
    } else if (wait == WAIT_OBJECT_0 && FlushFileBuffers(output) != 0 &&
               read_file(output, child_output) &&
               GetExitCodeProcess(child.hProcess, &exit_code) != 0 &&
               (exit_code == 0U || exit_code == 4U) &&
               [&child_output, exit_code, retrace_target, poll_target,
                play_mode] {
                 std::string canonical_line;
                 const bool valid =
                     exit_code == 0U
                         ? (play_mode
                                ? extract_canonical_play_exit(
                                      child_output, canonical_line)
                            : poll_target != 0U
                                ? extract_canonical_poll_probe(
                                      child_output, poll_target, canonical_line)
                            : retrace_target == 3U
                                ? extract_canonical_success(child_output,
                                                            canonical_line)
                                : (extract_canonical_probe(child_output, canonical_line) ||
                                   extract_original_os_probe(child_output, retrace_target, canonical_line)))
                         : extract_canonical_ledger(child_output,
                                                    canonical_line);
                  if (!valid) {
                    return false;
                  }
                 child_output = std::move(canonical_line);
                 return true;
               }()) {
      std::fwrite(child_output.data(), 1U, child_output.size(), stdout);
      result = static_cast<int>(exit_code);
    } else {
#if defined(JFG_PHASE8_LIVE_RUNTIME)
      if (!child_output.empty())
        std::fwrite(child_output.data(), 1U, child_output.size(), stderr);
      std::fprintf(stderr, "native boot child rejected: exit=0x%08lx\n",
                   static_cast<unsigned long>(exit_code));
#endif
      if (wait == WAIT_FAILED ||
          GetExitCodeProcess(child.hProcess, &exit_code) == 0) {
        result = 3;
      } else {
        result = static_cast<int>(exit_code == 2U ? 2U : 3U);
      }
    }
    if (GetExitCodeProcess(child.hProcess, &exit_code)) {
      char support_code[32]{};
      std::snprintf(support_code, sizeof(support_code), "native_exit=0x%08lx", static_cast<unsigned long>(exit_code));
      jfg::support_event(support_code);
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
  }
  CloseHandle(marker);
  CloseHandle(nul);
  CloseHandle(output);
  DeleteFileW(stdout_path);
  return result;
}
#endif

int run(const char *executable_path, const char *rom_path, unsigned timeout_ms,
        bool child_mode, const char *child_token, const char *marker_handle,
        unsigned retrace_target, unsigned poll_target, bool play_mode) {
  jfg::support_event("native=boot");
  if (child_mode) {
#if defined(_WIN32)
    if (!validate_child_capability(child_token, marker_handle))
      return 3;
#else
    return 3;
#endif
    return run_child(rom_path, retrace_target, poll_target, play_mode);
  }
#if defined(_WIN32)
  return run_parent(executable_path, rom_path, timeout_ms, retrace_target,
                    poll_target, play_mode);
#elif defined(JFG_HEADLESS_LIVE)
  // Headless measurement runs in-process; a crash ends the process.
  (void)executable_path;
  (void)timeout_ms;
  (void)child_token;
  (void)marker_handle;
  return run_child(rom_path, retrace_target, poll_target, play_mode);
#else
  (void)executable_path;
  (void)timeout_ms;
  std::fputs(
      "native boot setup failed: process isolation unavailable on this host\n",
      stderr);
  return 3;
#endif
}
} // namespace jfg::boot::native
