#include "jfg/audio/dma_backpressure.hpp"
#include "jfg/runtime/pi_dma_backpressure.hpp"
#include <iostream>

using jfg::audio::DmaQueueRequest;
using jfg::audio::requires_audio_dma_wait;
constexpr DmaQueueRequest audio_full{true, 0x80097104U, 0x80102538U,
    0x80102538U, 0x800F2898U, 11U, 1U, 0U, 8U, 8U};
constexpr bool scope_is_preserved() {
  auto r = audio_full;
  r.accelerated_clock = false;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.pending = 7U;
  if (!requires_audio_dma_wait(r)) return false;
  r = audio_full; r.pending = 9U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.capacity = 0U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.capacity = 9U; r.pending = 9U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.caller = 0x80097128U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.queue = 0U; r.pi_queue = 0U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.pi_queue = 0x800F1A58U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.reply_queue = 0x800FE000U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.message_type = 12U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.priority = 0U;
  if (requires_audio_dma_wait(r)) return false;
  r = audio_full; r.block_flag = 1U;
  return !requires_audio_dma_wait(r);
}
static_assert(requires_audio_dma_wait(audio_full));
static_assert(scope_is_preserved());
constexpr DmaQueueRequest asset_full{true, 0x80097124U, 0x80102538U,
    0x80102538U, 0x800FE000U, 11U, 0U, 0U, 8U, 8U};
constexpr bool asset_scope_is_preserved() {
  if (!jfg::requires_asset_dma_wait(asset_full) ||
      requires_audio_dma_wait(asset_full) || jfg::requires_asset_dma_wait(audio_full)) return false;
  auto r = asset_full;
  r.accelerated_clock = false;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.caller = 0x80097104U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.reply_queue = 0x800F2898U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.queue = 0U; r.pi_queue = 0U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.pi_queue = 0x800F1A58U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.message_type = 12U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.priority = 1U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.block_flag = 1U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.pending = 7U;
  if (!jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.pending = 9U;
  if (jfg::requires_asset_dma_wait(r)) return false;
  r = asset_full; r.capacity = 9U; r.pending = 9U;
  return !jfg::requires_asset_dma_wait(r);
}
static_assert(asset_scope_is_preserved());
// Model the original kernel's admission decision after interrupts are disabled.
// An interrupt between the host hook and that point can consume the last slot.
enum class Admission { immediate, wait, rejected };
constexpr Admission kernel_admission(unsigned pending, unsigned capacity, bool block) {
  return pending < capacity ? Admission::immediate :
      block ? Admission::wait : Admission::rejected;
}
constexpr bool interrupt_race_is_covered() {
  for (unsigned depth = 0; depth <= 8; ++depth) {
    auto audio = audio_full; audio.pending = depth;
    auto asset = asset_full; asset.pending = depth;
    if (!requires_audio_dma_wait(audio) || !jfg::requires_asset_dma_wait(asset)) return false;
    // Both same-depth admission and an intervening producer must preserve work.
    for (unsigned admitted_depth = depth; admitted_depth <= 8; ++admitted_depth) {
      const auto expected = admitted_depth < 8 ? Admission::immediate : Admission::wait;
      if (kernel_admission(admitted_depth, 8, requires_audio_dma_wait(audio)) != expected ||
          kernel_admission(admitted_depth, 8, jfg::requires_asset_dma_wait(asset)) != expected)
        return false;
    }
  }
  // The former full-snapshot-only policy loses a request in the 7 -> 8 case.
  return kernel_admission(8, 8, 7 == 8) == Admission::rejected;
}
static_assert(interrupt_race_is_covered());

int main() {
  if (!requires_audio_dma_wait(audio_full) || !scope_is_preserved() || !asset_scope_is_preserved() || !interrupt_race_is_covered()) return 1;
  std::cout << "Audio DMA backpressure scope checks passed\n";
}
