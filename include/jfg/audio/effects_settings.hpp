#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <string_view>
namespace jfg::audio {
struct EffectsSettings { bool target_acquisition = true; };
inline std::optional<EffectsSettings> parse_effects(std::string_view text) {
  if(text == "version=1\ntarget_acquisition=on\n" ||
     text == "version=1\r\ntarget_acquisition=on\r\n") return EffectsSettings{true};
  if(text == "version=1\ntarget_acquisition=off\n" ||
     text == "version=1\r\ntarget_acquisition=off\r\n") return EffectsSettings{false};
  return std::nullopt;
}
// Filter only the reticle crossing an enemy, including the multi-lock path.
// The character's lock-on voice cues and unrelated uses of cue 29 stay audible.
constexpr bool acquisition_entry(const std::uint32_t entry) noexcept {
  return entry==0x80001248U;
}
constexpr bool suppress_acquisition(const bool enabled, const std::uint32_t entry,
    const std::uint32_t sound, const std::uint32_t caller) noexcept {
  return !enabled && entry==0x80001248U && sound==0x1DU &&
    (caller==0x8007C0D8U || caller==0x8007C1D8U);
}
} // namespace jfg::audio
