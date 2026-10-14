#pragma once
#include <cstdint>

namespace jfg::audio {
struct DmaQueueRequest {
  bool accelerated_clock;
  std::uint32_t caller, queue, pi_queue, reply_queue;
  std::uint32_t message_type, priority, block_flag, pending, capacity;
};

// Supported US sound streaming uses high-priority reads and ignores rejection.
// Preserve its requests under the accelerated CPU without changing other queues.
[[nodiscard]] constexpr bool requires_audio_dma_wait(const DmaQueueRequest &r) noexcept {
  return r.accelerated_clock && r.caller == 0x80097104U &&
         r.queue != 0U && r.queue == r.pi_queue &&
         r.reply_queue == 0x800F2898U && r.message_type == 11U &&
         r.priority == 1U && r.block_flag == 0U &&
         r.capacity == 8U && r.pending == r.capacity;
}
} // namespace jfg::audio
