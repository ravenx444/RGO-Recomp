#include "jfg/audio/dma_backpressure.hpp"
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
  if (requires_audio_dma_wait(r)) return false;
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
int main() {
  if (!requires_audio_dma_wait(audio_full) || !scope_is_preserved()) return 1;
  std::cout << "Audio DMA backpressure scope checks passed\n";
}
