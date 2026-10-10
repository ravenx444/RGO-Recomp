#pragma once

// RGO headless measurement: start a new game without a player.
//
// The headless Linux build has no window or controller. This autopilot reads
// the game's front-end state from RDRAM on every controller poll and presses
// the buttons a player would, following scripts/phase95_startup.py: A on the
// title and file screens, A through the name keyboard (moving to END first
// once a default name is filled in). In gameplay it walks forward and turns
// now and then, so the measurement covers a moving scene. Because it reacts to game state rather than to VI numbers,
// it reaches the first level identically at every frame-rate setting.

#include <cstdint>
#include <optional>

namespace jfg::rgo {

inline constexpr std::uint32_t kFrontModeAddress = 0x000A51B0U;  // physical
inline constexpr std::uint32_t kLevelNumberAddress = 0x000FB114U;
inline constexpr std::uint32_t kRunlinkTableAddress = 0x000FEAA0U;
inline constexpr std::uint16_t kButtonA = 0x8000U;

// Big-endian guest reads by virtual address. Implementations cover KSEG0
// RDRAM (0x80000000-0x803FFFFF) and, in the native runtime, overlay memory at
// each overlay's linked address (overlay N at N << 20), where the recompiled
// overlays keep their code and data. They return std::nullopt elsewhere.
struct AutopilotMemory {
    virtual ~AutopilotMemory() = default;
    [[nodiscard]] virtual std::optional<std::uint8_t> u8(std::uint32_t address) const = 0;
    [[nodiscard]] virtual std::optional<std::uint32_t> u32(std::uint32_t address) const = 0;
};

struct AutopilotInput {
    std::uint16_t buttons = 0U;
    std::int8_t stick_x = 0;
    std::int8_t stick_y = 0;
};

struct KeyboardState {
    std::uint8_t submode = 0U;
    std::uint32_t key = 0U;
    std::uint32_t label_length = 0U;
};

inline constexpr std::uint32_t kKeyboardOverlay = 42U;
inline constexpr std::uint32_t kKeyboardOverlayVram = kKeyboardOverlay << 20U;

// The name keyboard is overlay 42. The runlink table gives its relocated
// copy in RDRAM; its variables are recovered from that copy's LUI/ADDIU
// pairs, as in scripts/phase95_keyboard.py, then read at the same offset
// from the overlay's linked address, where the recompiled code keeps them.
[[nodiscard]] inline std::optional<KeyboardState> read_keyboard(
    const AutopilotMemory& memory) {
    const auto table = memory.u32(0x80000000U | kRunlinkTableAddress);
    if (!table || *table < 0x80000000U || *table >= 0x80400000U) return std::nullopt;
    const auto base = memory.u32(*table + kKeyboardOverlay * 32U);
    if (!base || *base < 0x80000000U || *base >= 0x80400000U - 0x2078U) return std::nullopt;
    const auto linked = [&](std::uint32_t pointer) -> std::optional<std::uint32_t> {
        if (pointer >= *base && pointer < *base + 0x10000U)
            return kKeyboardOverlayVram + (pointer - *base);
        if (pointer >= kKeyboardOverlayVram && pointer < kKeyboardOverlayVram + 0x10000U)
            return pointer;
        return std::nullopt;
    };
    const auto address = [&](std::uint32_t hi, std::uint32_t lo, std::uint32_t hi_op,
                             std::uint32_t lo_op) -> std::optional<std::uint32_t> {
        const auto first = memory.u32(*base + hi);
        const auto second = memory.u32(*base + lo);
        if (!first || !second || (*first >> 16U) != hi_op || (*second >> 16U) != lo_op)
            return std::nullopt;
        const auto low = static_cast<std::int16_t>(*second & 0xFFFFU);
        return linked(((*first & 0xFFFFU) << 16U) +
                      static_cast<std::uint32_t>(static_cast<std::int32_t>(low)));
    };
    const auto initialized = address(0U, 4U, 0x3C02U, 0x2442U);
    const auto pointer = address(0x84U, 0x8CU, 0x3C02U, 0x2442U);
    const auto label = address(0x88U, 0x90U, 0x3C08U, 0x2508U);
    const auto submode = address(0x9CU, 0xA0U, 0x3C01U, 0xA020U);
    const auto key = address(0x206CU, 0x2070U, 0x3C18U, 0x8F18U);
    if (!initialized || !pointer || !label || !submode || !key) return std::nullopt;
    if (memory.u32(*initialized).value_or(0U) != 1U) return std::nullopt;
    const auto end = memory.u32(*pointer);
    if (!end || *end == 0U) return std::nullopt;
    const auto end_linked = linked(*end);
    const auto selection = memory.u32(*key);
    const auto mode = memory.u8(*submode);
    if (!end_linked || !selection || !mode || *selection > 40U ||
        *end_linked < *label || *end_linked - *label > 23U)
        return std::nullopt;
    return KeyboardState{*mode, *selection, *end_linked - *label};
}

class NewGameAutopilot {
public:
    // One decision per controller poll. Each press is held for a few polls
    // and then released, so the game sees a new press every time.
    AutopilotInput sample(const AutopilotMemory& memory) {
        if (hold_ > 0U) {
            --hold_;
            return held_;
        }
        if (release_ > 0U) {
            --release_;
            return {};
        }
        const auto mode = memory.u8(0x80000000U | kFrontModeAddress).value_or(0U);
        if (mode == 3U || mode == 5U)
            return press({kButtonA, 0}, 6U, 15U);
        if (mode == 24U) {
            const auto keyboard = read_keyboard(memory);
            // Mode 24 opens on the file screen: choose the (empty) file.
            if (!keyboard) return press({kButtonA, 0}, 6U, 60U);
            if (keyboard->submode == 0U ||
                keyboard->label_length == 0U || keyboard->key == 40U)
                return press({kButtonA, 0}, 6U, 15U);
            if (keyboard->submode == 1U) {
                // Keys wrap over 41 entries; END is key 40.
                const std::int8_t direction = keyboard->key < 20U ? -80 : 80;
                return press({0U, direction}, 3U, 6U);
            }
        }
        // In gameplay (mode 16), walk forward and turn now and then, so the
        // camera and the scene keep moving. Cutscenes ignore the stick.
        if (mode == 16U) {
            ++gameplay_polls_;
            const std::uint32_t phase = gameplay_polls_ % 150U;
            if (phase < 120U) return {0U, 0, 70};
            return {0U, 60, 0};
        }
        gameplay_polls_ = 0U;
        return {};
    }

private:
    AutopilotInput press(AutopilotInput input, std::uint32_t hold, std::uint32_t release) {
        held_ = input;
        hold_ = hold - 1U;
        release_ = release;
        return input;
    }
    AutopilotInput held_{};
    std::uint32_t hold_ = 0U;
    std::uint32_t release_ = 0U;
    std::uint32_t gameplay_polls_ = 0U;
};

}  // namespace jfg::rgo
