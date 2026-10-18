#pragma once
#include "jfg/audio/dma_backpressure.hpp"

namespace jfg {
// romCopy submits a normal-priority cartridge read, ignores rejection, then
// blocks waiting for its one-slot completion queue. Under the accelerated
// clock the PI command queue can fill before the manager drains it. Let the
// original guest kernel wait for admission rather than lose the request.
// Queue depth is only a snapshot before the kernel disables interrupts, so
// waiting must also be permitted when that snapshot is below capacity.
[[nodiscard]] constexpr bool requires_asset_dma_wait(
    const audio::DmaQueueRequest &r) noexcept {
  return r.accelerated_clock && r.caller == 0x80097124U &&
         r.queue != 0U && r.queue == r.pi_queue &&
         r.reply_queue == 0x800FE000U && r.message_type == 11U &&
         r.priority == 0U && r.block_flag == 0U &&
         r.capacity == 8U && r.pending <= r.capacity;
}
} // namespace jfg
