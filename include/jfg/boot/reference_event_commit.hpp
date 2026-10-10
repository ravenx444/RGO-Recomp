#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace jfg::boot {
enum class ReferenceEvent { vi, pi, si, compare, sp, dp, ai };
struct ReferenceEventCandidate { ReferenceEvent source; std::uint64_t deadline; };
struct ReferenceEventSelection { bool qualified = true; std::optional<ReferenceEvent> source; };

// The independent SP/DP experiment observes one completion per CPU branch
// boundary, including equal-deadline SP then DP. Other simultaneous-source
// orders are not guessed. Different deadlines are always chronological.
//
// RGO fast clock (order_ties): Count advances by a fraction of a tick per
// instruction, so two devices started a few instructions apart can share a
// deadline. Real hardware orders them by a few cycles; the fast clock has no
// such resolution, so it takes the earlier-listed source instead of failing.
inline ReferenceEventSelection select_reference_event(
    std::span<const ReferenceEventCandidate> candidates, std::uint64_t count,
    bool order_ties = false) {
  ReferenceEventSelection selected;
  std::uint64_t earliest = UINT64_MAX;
  for (const auto candidate : candidates) {
    if (candidate.deadline > count) continue;
    if (!selected.source || candidate.deadline < earliest) {
      earliest = candidate.deadline;
      selected = {true, candidate.source};
    } else if (candidate.deadline == earliest) {
      if ((*selected.source == ReferenceEvent::sp && candidate.source == ReferenceEvent::dp) ||
          (*selected.source == ReferenceEvent::dp && candidate.source == ReferenceEvent::sp))
        selected.source = ReferenceEvent::sp;
      else if (!order_ties) selected.qualified = false;
    }
  }
  return selected;
}
} // namespace jfg::boot
